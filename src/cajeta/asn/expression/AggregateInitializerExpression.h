// `Foo { field: expr, ... }`: allocates a zero-initialized `Foo` — so omitted
// fields land at 0 — stores each labeled expression into its struct slot, and
// returns the pointer. Labels are required, and must name declared properties.

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"

namespace cajeta {

    class AggregateInitializerExpression : public Expression {
        string typeName;
        // MethodCallParameter, since keyword arguments share the `parameterList` rule.
        vector<MethodCallParameter> bindings;
        // True for `Foo { ... }` and `stack Foo { ... }`; false for `heap Foo { ... }`,
        // whose path also initializes the vtable slot when the class has one.
        bool stackAlloc = true;
        // Target type pushed in by the surrounding context for the prefixless
        // `{...}` form. An explicit `typeName` prefix always wins over it.
        CajetaTypePtr expectedType;
    public:
        void setStackAlloc(bool v) { stackAlloc = v; }
        bool getStackAlloc() const { return stackAlloc; }
        void setExpectedType(CajetaTypePtr t) { expectedType = std::move(t); }

        AggregateInitializerExpression(
                CajetaParser::AggregateInitializerContext* ctx,
                antlr4::Token* token)
            : Expression(token) { exprKind = ExprKind::Aggregate;
            typeName = ctx->identifier() ? ctx->identifier()->getText() : "";
            if (ctx->parameterList()) {
                for (auto& entryCtx : ctx->parameterList()->parameterEntry()) {
                    MethodCallParameter b;
                    b.expression = Expression::fromContext(entryCtx->expression());
                    if (entryCtx->parameterLabel()) {
                        b.label = entryCtx->parameterLabel()->getText();
                        // parameterLabel's getText() includes the trailing `:`.
                        if (!b.label.empty() && b.label.back() == ':') {
                            b.label.pop_back();
                        }
                    }
                    if (entryCtx->REFERENCE()) {
                        b.callerTransferred = true;
                    }
                    bindings.push_back(b);
                }
            }
        }

        const string& getTypeName() const { return typeName; }
        const vector<MethodCallParameter>& getBindings() const { return bindings; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // namespace cajeta
