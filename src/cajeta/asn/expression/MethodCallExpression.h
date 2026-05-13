//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {

    struct MethodCallParameter {
        string label;
        ExpressionPtr expression;
    };

    class MethodCallExpression : public Expression {
        string methodCallName;
        vector<MethodCallParameter> parameters;
    public:
        MethodCallExpression(CajetaParser::MethodCallContext* ctx, antlr4::Token* token) : Expression(token) {
            methodCallName = ctx->identifier()->getText();
            if (ctx->parameterList()) {
                for (auto& ctxParameterEntry: ctx->parameterList()->parameterEntry()) {
                    MethodCallParameter entry;
                    entry.expression = Expression::fromContext(ctxParameterEntry->expression());
                    if (ctxParameterEntry->parameterLabel()) {
                        entry.label = ctxParameterEntry->parameterLabel()->getText();
                    }
                    parameters.push_back(entry);
                }
            }
        }

        // Method-call args aren't in `children` (children[0] is the receiver,
        // if any). The free-variable walk in LambdaExpression uses this to
        // recurse into the args when scanning a lambda body for captures.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        /**
         * First, get the full name of the object.
         * @param module
         * @return
         */
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code