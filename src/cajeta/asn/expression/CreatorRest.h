//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"

namespace cajeta {

    class CreatorRest : public AbstractSyntaxNode {
    public:
        CreatorRest(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        static CreatorRest* fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token);
    };

    class ClassCreatorRest : public CreatorRest {
        vector<MethodCallParameter> parameters;
    public:
        ClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            if (ctx->arguments()->parameterList()) {
                for (auto& ctxParameterEntry: ctx->arguments()->parameterList()->parameterEntry()) {
                    MethodCallParameter entry;
                    entry.label = ctxParameterEntry->getText();
                    entry.expression = Expression::fromContext(ctxParameterEntry->expression());
                    parameters.push_back(entry);
                }
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class ArrayCreatorRest : public CreatorRest {
    private:
    public:
        ArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            for (auto& expressionContext: ctx->expression()) {
                children.push_back(Expression::fromContext(expressionContext));
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code