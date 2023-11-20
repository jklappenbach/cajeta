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

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code