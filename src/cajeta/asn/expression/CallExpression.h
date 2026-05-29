//
// Postfix call applied to the result of an expression.
//

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"   // MethodCallParameter

namespace cajeta {

    // `<callee>(args)` — a call applied to whatever an expression evaluates
    // to, as opposed to MethodCallExpression which names a method directly.
    // Grammar: `expression '(' parameterList? ')'` (CajetaParser.g4), added
    // for the XPU launch form
    //
    //     kernel.launch(stream, grid: [...], block: [...])(kernelArgs)
    //
    // where the callee is the `.launch(config)` method call and the trailing
    // `(kernelArgs)` binds the kernel parameters. The form is general by
    // design: any expression yielding a callable can be invoked this way.
    //
    // The callee lands in `children[0]` via Expression::fromContext's child
    // loop; the argument list is parsed in the constructor (same shape as
    // MethodCallExpression's `parameterList` handling). Labels keep their
    // trailing colon, matching MethodCallExpression — callers strip it.
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
