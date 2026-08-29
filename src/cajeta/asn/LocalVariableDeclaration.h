//
// Created by James Klappenbach on 11/4/22.
//

#pragma once

#include "VariableDeclarator.h"
#include "../type/CajetaType.h"
#include "BlockStatement.h"

namespace cajeta {

    class LocalVariableDeclaration : public BlockStatement {
    private:
        set<Modifier> modifiers;
        set<QualifiedNamePtr> annotations;
        CajetaTypePtr type;
        list<VariableDeclaratorPtr> variableDeclarators;
    public:
        // Drop-chain wiring for an owner local, callable from OTHER emission
        // sites that create ownership after declaration (e.g. a `d = #t`
        // move-assign into a bare-declared local — slices plan 9.3.1). Binds
        // the CURRENT slot value as the entry's obj, so call it after the
        // store.
        static void emitOwnerDropEntry(CajetaModulePtr module, FieldPtr field,
            const std::string& dropFnName, int allocLine);

        // xref-lint-emission-gap Unit 3 — register this declaration's bindings
        // during a RESOLVE-ONLY walk, so a later `local.field` has a typed
        // receiver. No-op in a build: there, generateCode registers the real
        // (slot-carrying) field and this would only shadow it.
        void resolveTypes(CajetaModulePtr module) override;

        LocalVariableDeclaration(set<Modifier>& modifiers,
            CajetaTypePtr type,
            list<VariableDeclaratorPtr> variableDeclarators,
            antlr4::Token* token) : BlockStatement(token) {
            this->modifiers = modifiers;
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }

        // For tree walkers that need to reach sub-expressions hidden in the
        // private declarator list (e.g. the lambda body's free-variable
        // scan). The walker visits each declarator's initializer to find
        // identifiers referenced on the RHS.
        const list<VariableDeclaratorPtr>& getVariableDeclarators() const {
            return variableDeclarators;
        }

        // Declared type for the local. May be null for `var`-style
        // declarations whose type is inferred from the initializer.
        // Used by lambda return-type inference to pre-register body
        // locals in the lambda's resolve-time scope.
        CajetaTypePtr getType() const { return type; }

        // 7.2.4 — declarators are private; each declarator's own children
        // carry its initializer.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            for (auto& d : variableDeclarators) {
                if (d) fn(d);
            }
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code