//
// Created by James Klappenbach on 11/4/22.
//

#pragma once
#include <cajeta/asn/VariableDeclarator.h>
#include <cajeta/type/CajetaType.h>
#include <cajeta/asn/BlockStatement.h>

namespace cajeta {

    class LocalVariableDeclaration : public BlockStatement {
    private:
        set<Modifier> modifiers;
        CajetaType* type;
        list<VariableDeclarator*> variableDeclarators;
    public:
        LocalVariableDeclaration(set<Modifier>& modifiers,
                                 CajetaType* type,
                                 list<VariableDeclarator*> variableDeclarators,
                                 antlr4::Token* token) : BlockStatement(token) {
            this->modifiers = modifiers;
            this->type = type;
            this->variableDeclarators = variableDeclarators;
        }

        llvm::Value* generateCode(CajetaModule* compilationUnit) override;
    };

} // cajeta