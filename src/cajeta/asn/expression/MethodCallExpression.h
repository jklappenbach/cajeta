//
// Created by James Klappenbach on 4/19/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {

    class CajetaFunctionType;
    class CajetaClass;

    struct MethodCallParameter {
        string label;
        ExpressionPtr expression;
        // Phase 1 of two-sided transfer (docs/specification/lang/OwnershipTransfer.md).
        // `#x` at the argument position sets this. ONLY this flag fires the
        // drop deactivation in MethodCallExpression.cpp / CreatorRest.cpp —
        // the call-site `#` always transfers and is always sufficient. A `#T`
        // FORMAL never transfers on its own: when the formal is `#T` and this
        // flag is false, the callee-side pass raises
        // CAJETA_ERROR_TRANSFER_REQUIRED, compelling the caller to write `#`.
        // The two sides are asymmetric — the formal's `#` is an obligation,
        // not an acknowledgement.
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
    // place. See docs/specification/lang/Lambdas.md.
    // When `directFn` is non-null the call targets that known function directly
    // (closure-specialization fast path, cajeta-ir Unit 4): captures fold to
    // null and `closurePtr` is ignored — no indirect load-through-record.
    llvm::Value* emitClosureCall(CajetaModulePtr module,
                                 llvm::Value* closurePtr,
                                 const std::shared_ptr<CajetaFunctionType>& fnType,
                                 const vector<MethodCallParameter>& args,
                                 CajetaTypePtr& outResolvedType,
                                 llvm::Function* directFn = nullptr);

    class MethodCallExpression : public Expression {
        string methodCallName;
        vector<MethodCallParameter> parameters;
    public:
        // transform-intrinsics U7 — a compiler-built bare combinator call (the
        // annotation-sugar desugar). No parser context; the caller stamps the
        // desugared call site's span via setSourceSpan.
        MethodCallExpression(string name, vector<MethodCallParameter> params)
            : Expression(nullptr), methodCallName(std::move(name)),
              parameters(std::move(params)) { }
    private:
        // title-tracking 6.2.2 — set by the Cajeta.flagged(v, owned)
        // intrinsic: the runtime i64 title flag paired with this call's
        // value. ReturnStatement reads it to thread a container's MANUAL
        // ownership bookkeeping (HashMap's owned[] bits) onto the return
        // flag — cajeta code cannot otherwise mint "owned iff my bit says
        // so". Null for every ordinary call.
        llvm::Value* flaggedTitleValue = nullptr;
        // True for the `super(args)` methodCall alternative (CajetaParser.g4:630).
        // The ordinary identifier path doesn't set this; the SUPER form does so
        // generateCode can route through the parent class's constructor instead
        // of doing identifier-based dispatch (and ctx->identifier() is null in
        // this case so the usual `getText()` call would null-deref).
        bool superCtorCall = false;
        // Explicit method-level template type arguments from the
        // `identifier<TypeArgs>(args)` call-site syntax (Form C). Empty
        // for ordinary calls (type args inferred via unification at
        // resolveMethod time). See docs/specification/lang/templates/MethodLevelTemplate.md.
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
        // REFL-12: idempotency guard for the bounded-reflection lowering.
        // `Class.newInstance<Shape>(name)` / `Class.forName<Shape>(name)` are
        // rewritten by appending the bound (`Shape.class`) as a synthesized
        // second argument so the call resolves to the 2-arg bounded overload.
        // generateCode may run more than once; this ensures the arg is injected
        // exactly once.
        bool boundedReflInjected = false;
        // element-ownership 3.4.3: whether the resolved callee declares an
        // ownership-transferring (`#T`) return. Set during generateCode's
        // resolution; an enclosing call site consults it to classify this
        // call's result as a fresh owned temporary that the enclosing
        // statement must reclaim when consumed as a borrow argument.
        // Stays false on intrinsic paths that never resolve a user
        // method — conservative (no reclamation).
        bool resolvedReturnsOwnership = false;
        // 6.2.2 — the method this call resolved to (set beside
        // resolvedReturnsOwnership); lets statement-position consumers ask
        // shape questions (returnsClassPointer) without re-resolving.
        MethodPtr resolvedMethod;
    public:
        bool isResolvedReturnsOwnership() const { return resolvedReturnsOwnership; }
        llvm::Value* getFlaggedTitleValue() const { return flaggedTitleValue; }
        MethodPtr getResolvedMethod() const { return resolvedMethod; }

        // element-ownership 3.4.3 / slices 9.4.1 — statement-end temp
        // classification, shared with the ctor-arg site (ClassCreatorRest).
        // freshOwnedStringTemp: anonymous owned-String rvalue (inline concat
        // or #String-returning call). freshSharedValueTempClass: a call
        // result of a shared-capable VALUE type (Utf8 / Slice / aggregates
        // embedding them) — every such rvalue carries its stakes with the
        // bytes; returns the class for the release emit, or null.
        static bool freshOwnedStringTemp(const AbstractSyntaxNodePtr& e);
        static shared_ptr<CajetaClass> freshSharedValueTempClass(
            const AbstractSyntaxNodePtr& e);
        // title-tracking 6.2.5 — temps whose title rides the transfer word /
        // caller-side reclaim: concrete vtable classes only (Strings keep
        // 3.4.3, values 9.4.1, interfaces have no entry to take a title).
        static shared_ptr<CajetaClass> droppableTempClass(
            const CajetaTypePtr& t);
        // A plain `heap X(...)` creator — an anonymous owned rvalue that
        // surrenders per spec §4.1.1. stack/shared placements never do.
        static shared_ptr<CajetaClass> freshHeapCreatorTempClass(
            const AbstractSyntaxNodePtr& e);
        // The array-typed twin: a heap `[1, 2]` literal argument is equally an
        // anonymous owned rvalue. stack/arena literals never surrender — the
        // frame reclaims their storage.
        static bool freshHeapArrayLiteralArg(const AbstractSyntaxNodePtr& e);
        CajetaTypePtr getPreProjectionReturnType() const {
            return preProjectionReturnType;
        }
        MethodCallExpression(CajetaParser::MethodCallContext* ctx, antlr4::Token* token);

        // Method-call args aren't in `children` (children[0] is the receiver,
        // if any). The free-variable walk in LambdaExpression uses this to
        // recurse into the args when scanning a lambda body for captures.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        // 7.2.4 — args are private; children[0] is only the receiver.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            for (auto& p : parameters) {
                if (p.expression) fn(p.expression);
            }
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        const string& getMethodCallName() const { return methodCallName; }

        // Conservative callee peek for ARGUMENT-position calls, used by the
        // `#T`-formal transfer checks (here and CreatorRest) to classify a
        // call-result argument as owned-returning (`-> #R`) or a borrow
        // BEFORE the arg has generated (resolvedMethod is only set during
        // the call's own codegen). Resolves the receiver class (bare call ->
        // enclosing class; `X.m()` -> X as a type name; `expr.m()` -> expr's
        // resolved type) and answers ONLY on a unique name+arity match —
        // ambiguity or an unresolvable receiver returns null so the caller
        // stays conservative (no false rejections).
        static MethodPtr resolveArgCalleeShallow(
            const std::shared_ptr<MethodCallExpression>& call,
            CajetaModulePtr module);
        void setMethodCallName(const string& name) { methodCallName = name; }
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