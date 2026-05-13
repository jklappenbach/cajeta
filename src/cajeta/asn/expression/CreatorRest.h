//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"

namespace cajeta {

    class CreatorRest : public AbstractSyntaxNode {
    protected:
        // Target type set by the parent NewExpression before generateCode runs.
        // For ClassCreatorRest this is the struct type; for ArrayCreatorRest the
        // element type.
        CajetaTypePtr targetType;
    public:
        CreatorRest(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        void setTargetType(CajetaTypePtr t) { targetType = t; }

        static shared_ptr<CreatorRest> fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token);
    };
    typedef shared_ptr<CreatorRest> CreatorRestPtr;

    class ClassCreatorRest : public CreatorRest {
        vector<MethodCallParameter> parameters;
    public:
        ClassCreatorRest(CajetaParser::ClassCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            if (ctx->arguments()->parameterList()) {
                for (auto& ctxParameterEntry: ctx->arguments()->parameterList()->parameterEntry()) {
                    MethodCallParameter entry;
                    entry.expression = Expression::fromContext(ctxParameterEntry->expression());
                    // Only set label if a parameterLabel actually exists. Reading
                    // ctxParameterEntry->getText() captures the entire entry text
                    // (including the expression), which would make every positional
                    // arg look labeled and route through the labeled-ctor resolver
                    // — silently failing the lookup.
                    if (ctxParameterEntry->parameterLabel()) {
                        entry.label = ctxParameterEntry->parameterLabel()->getText();
                    }
                    parameters.push_back(entry);
                }
            }
        }

        // Read-only access to the constructor-call argument list, exposed so
        // TPL-7 diamond inference can inspect arg types without re-evaluating
        // expressions.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class ArrayCreatorRest : public CreatorRest {
    private:
        // Total `[]` pairs in the creator (e.g. `new T[2][3][]` has 3). Equals the
        // nesting depth of the resulting array type. children.size() is the number
        // of levels with explicit sizes — the rest are left null after allocation.
        int totalBracketPairs;
    public:
        ArrayCreatorRest(CajetaParser::ArrayCreatorRestContext* ctx, antlr4::Token* token) : CreatorRest(token) {
            totalBracketPairs = static_cast<int>(ctx->LBRACK().size());
            for (auto& expressionContext: ctx->expression()) {
                children.push_back(Expression::fromContext(expressionContext));
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code