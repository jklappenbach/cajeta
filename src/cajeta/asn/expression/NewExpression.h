//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"
#include "CreatorRest.h"

namespace cajeta {

    class NewExpression : public Expression {
        string package;
        string typeName;
        CreatorRest* creatorRest;
    public:
        NewExpression(antlr4::Token* token) : Expression(token) { }

        NewExpression(CajetaParser::CreatorContext* creatorContext, antlr4::Token* token) : Expression(token) {
            if (creatorContext->createdName() != nullptr) {
                if (creatorContext->createdName()->primitiveType()) {
                    typeName = creatorContext->createdName()->primitiveType()->getText();
                } else if (!creatorContext->createdName()->identifier().empty()) {
                    int count = creatorContext->createdName()->identifier().size();
                    int n = 0;
                    for (auto& identifierPart: creatorContext->createdName()->identifier()) {
                        if (n++ == count - 1) {
                            typeName = identifierPart->getText();
                        } else {
                            package.append(identifierPart->getText());
                        }
                    }
                }
            }
            creatorRest = CreatorRest::fromContext(creatorContext, token);
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code