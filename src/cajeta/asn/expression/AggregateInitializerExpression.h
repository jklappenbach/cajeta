//
// AggregateInitializerExpression — `Foo { field: expr, ... }`.
//
// Builds a fresh stack alloca of struct `Foo`, zero-inits the body (so
// fields the initializer omits land at 0), then stores each labeled
// expression into the corresponding LLVM struct slot. Returns the alloca
// pointer so the receiving site (local declaration, function argument,
// return slot) handles it like any other aggregate-by-pointer value.
//
// S6.2 scope: receiver type must be a CajetaStruct (not CajetaView, not
// a plain CajetaClass). Field names must match declared properties; the
// labelled-binding form is required (positional aggregate init isn't
// supported in v1 — labels match Structs.md's documented syntax and
// match the keyword-arg parser shape we already accept on methodCall).
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
    public:
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
