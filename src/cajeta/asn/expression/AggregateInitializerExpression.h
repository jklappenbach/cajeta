//
// AggregateInitializerExpression — `Foo { field: expr, ... }`.
//
// Builds a fresh stack alloca of class `Foo`, zero-inits the body (so
// fields the initializer omits land at 0), then stores each labeled
// expression into the corresponding LLVM struct slot. Returns the alloca
// pointer so the receiving site (local declaration, function argument,
// return slot) handles it like any other class-by-pointer value.
//
// Receiver type must be a CajetaClass (not CajetaView). Field names
// must match declared properties; the labelled-binding form is required
// (positional aggregate init isn't supported in v1 — labels match the
// keyword-arg parser shape already accepted on methodCall).
//

#pragma once

#include "Expression.h"
#include "MethodCallExpression.h"

namespace cajeta {

    class AggregateInitializerExpression : public Expression {
        string typeName;
        // Reuse MethodCallParameter so we share the label+expression shape
        // with method-call keyword arguments; the parser surface is the
        // same `parameterList` rule.
        vector<MethodCallParameter> bindings;
        // P2a: when true (default), allocates the body via stack alloca
        // — bare aggregate-init `Foo { ... }` and `stack Foo { ... }`.
        // When false, allocates via malloc — `heap Foo { ... }`. The
        // heap path also initializes the vtable slot if the target is a
        // CajetaClass with one.
        bool stackAlloc = true;
    public:
        void setStackAlloc(bool v) { stackAlloc = v; }
        bool getStackAlloc() const { return stackAlloc; }

        AggregateInitializerExpression(
                CajetaParser::AggregateInitializerContext* ctx,
                antlr4::Token* token)
            : Expression(token) {
            typeName = ctx->identifier()->getText();
            if (ctx->parameterList()) {
                for (auto& entryCtx : ctx->parameterList()->parameterEntry()) {
                    MethodCallParameter b;
                    b.expression = Expression::fromContext(entryCtx->expression());
                    if (entryCtx->parameterLabel()) {
                        b.label = entryCtx->parameterLabel()->getText();
                        // parameterLabel includes the trailing `:` token in
                        // its getText(); strip it so the label matches a
                        // bare field name.
                        if (!b.label.empty() && b.label.back() == ':') {
                            b.label.pop_back();
                        }
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
