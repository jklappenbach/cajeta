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
        // True for the `super(args)` methodCall alternative (CajetaParser.g4:630).
        // The ordinary identifier path doesn't set this; the SUPER form does so
        // generateCode can route through the parent class's constructor instead
        // of doing identifier-based dispatch (and ctx->identifier() is null in
        // this case so the usual `getText()` call would null-deref).
        bool superCtorCall = false;
        // Explicit method-level template type arguments from the
        // `identifier<TypeArgs>(args)` call-site syntax (Form C). Empty
        // for ordinary calls (type args inferred via unification at
        // resolveMethod time). See cajeta-docs/stdlib/MethodLevelTemplate.md.
        vector<CajetaTypePtr> explicitMethodTypeArgs;
    public:
        MethodCallExpression(CajetaParser::MethodCallContext* ctx, antlr4::Token* token);

        // Method-call args aren't in `children` (children[0] is the receiver,
        // if any). The free-variable walk in LambdaExpression uses this to
        // recurse into the args when scanning a lambda body for captures.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        const string& getMethodCallName() const { return methodCallName; }
        bool isSuperCtorCall() const { return superCtorCall; }
        const vector<CajetaTypePtr>& getExplicitMethodTypeArgs() const {
            return explicitMethodTypeArgs;
        }

        /**
         * First, get the full name of the object.
         * @param module
         * @return
         */
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code