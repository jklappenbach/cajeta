// Postfix call applied to the result of an expression.

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"   // MethodCallParameter

namespace cajeta {

    // `<callee>(args)` — a call applied to whatever an expression evaluates to,
    // as opposed to MethodCallExpression, which names a method directly; the XPU
    // launch form `k.launch(config)(args)` is one instance. Callee is children[0].
    class CallExpression : public Expression {
        vector<MethodCallParameter> args;
    public:
        CallExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        const vector<MethodCallParameter>& getArgs() const { return args; }

        // The invoked expression — children[0], attached after construction.
        ExpressionPtr getCallee() const {
            return children.empty()
                ? nullptr
                : dynamic_pointer_cast<Expression>(children[0]);
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // namespace cajeta
