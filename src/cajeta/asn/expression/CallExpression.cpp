//
// Postfix call applied to the result of an expression.
//

#include "CallExpression.h"
#include "cajeta/error/Exception.h"

namespace cajeta {

    CallExpression::CallExpression(
        CajetaParser::ExpressionContext* ctx,
        antlr4::Token* token) : Expression(token) {
        // Mirror MethodCallExpression's parameterList handling: each entry is
        // an optional label (kept with its trailing ':') plus an expression.
        if (auto* paramList = ctx->parameterList()) {
            for (auto& ctxParameterEntry : paramList->parameterEntry()) {
                MethodCallParameter entry;
                entry.expression = Expression::fromContext(
                    ctxParameterEntry->expression());
                if (ctxParameterEntry->parameterLabel()) {
                    entry.label = ctxParameterEntry->parameterLabel()->getText();
                }
                args.push_back(entry);
            }
        }
    }

    llvm::Value* CallExpression::generateCode(CajetaModulePtr module) {
        // Codegen lands with the XPU launch lowering (CajetaXPU step ~E):
        // recognize the `kernel.launch(config)(args)` shape and emit the
        // host-side CUDA launch. Until then a postfix call has no general
        // callable-value lowering, so surface a clear diagnostic rather than
        // emitting null IR.
        throw Exception(
            "postfix call expression (kernel.launch(...)(...) lowering is not "
            "yet implemented)",
            "CAJETA_ERROR_NOT_IMPLEMENTED");
    }

} // namespace cajeta
