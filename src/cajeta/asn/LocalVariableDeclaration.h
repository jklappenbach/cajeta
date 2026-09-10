// LocalVariableDeclaration - one `T a, b = e;` statement in a block.

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
        // Drop-chain wiring for an owner local, also callable from emission sites
        // that create ownership after the declaration. It binds the CURRENT slot
        // value as the entry's obj, so call it AFTER the store.
        static void emitOwnerDropEntry(CajetaModulePtr module, FieldPtr field,
            const std::string& dropFnName, int allocLine);

        // Register these bindings during a RESOLVE-ONLY walk, so a later
        // `local.field` has a typed receiver. A no-op in a build.
        void resolveTypes(CajetaModulePtr module) override;

        LocalVariableDeclaration(set<Modifier>& modifiers,
            CajetaTypePtr type,
            list<VariableDeclaratorPtr> variableDeclarators,
            antlr4::Token* token) : BlockStatement(token) {
            this->modifiers = modifiers;
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }

        // For tree walkers reaching into the private declarator list.
        const list<VariableDeclaratorPtr>& getVariableDeclarators() const {
            return variableDeclarators;
        }

        // Null for a `var`-style declaration, whose type the initializer decides.
        CajetaTypePtr getType() const { return type; }

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