//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {

    class CajetaFunctionType;

    struct MethodCallParameter {
        string label;
        ExpressionPtr expression;
        // Phase 1 of two-sided transfer (docs/stdlib/OwnershipTransfer.md).
        // `#x` at the argument position sets this; the call-site transfer
        // machinery in MethodCallExpression.cpp / CreatorRest.cpp fires the
        // drop deactivation when EITHER this is true OR the matching formal
        // is `#T`-marked. Either suffices; either acknowledges transfer.
        bool callerTransferred = false;
    };

    // Emit an indirect call through a closure value of function type `fnType`.
    // `closurePtr` is a `ptr` to the closure record `{ ptr fn, ptr captures,
    // ptr drop }` (the L3-3 ABI). `args` are the source-level argument
    // expressions (labels ignored). On an sret value-return this allocates the
    // result slot, threads it as the hidden arg 0, sets `outResolvedType` to the
    // function's return type, and returns the slot; otherwise it returns the
    // call result. Shared by the bare-identifier indirect call
    // (`op(args)` in MethodCallExpression) and the postfix expression/indexed
    // call (`arr[i](args)` in CallExpression) so the closure ABI lives in one
    // place. See docs/stdlib/Lambdas.md.
    llvm::Value* emitClosureCall(CajetaModulePtr module,
                                 llvm::Value* closurePtr,
                                 const std::shared_ptr<CajetaFunctionType>& fnType,
                                 const vector<MethodCallParameter>& args,
                                 CajetaTypePtr& outResolvedType);

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
        // resolveMethod time). See docs/stdlib/MethodLevelTemplate.md.
        vector<CajetaTypePtr> explicitMethodTypeArgs;
        // Capture identity for the read-back pattern. `resolvedType` is
        // the projected bound (via captureProject) for user-facing
        // chained-member resolution; `preProjectionReturnType` is the
        // un-projected wildcard sentinel. The outer call site uses
        // the latter to detect "value came from the same wildcard
        // receiver" — wildcard meets wildcard at the parameter check
        // and the call resolves cleanly. v1 scope: syntactic
        // receiver-identifier equality.
        CajetaTypePtr preProjectionReturnType;
    public:
        CajetaTypePtr getPreProjectionReturnType() const {
            return preProjectionReturnType;
        }
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