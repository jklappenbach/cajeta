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

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code