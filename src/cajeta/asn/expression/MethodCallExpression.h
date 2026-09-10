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
        // Set by `#x` at the argument position, the only thing that deactivates the
        // source's drop entry: a `#T` formal obliges the caller, but never transfers.
        bool callerTransferred = false;
    };

    // Applies the transfer-of-a-borrow rejection to every `#`-marked ARGUMENT, for calls
    // and construction alike: an argument's `#` builds no MoveExpression, where the rest
    // of the borrow checks live.
    void rejectTransferOfBorrowArgs(CajetaModulePtr module,
                                    const vector<MethodCallParameter>& args);

    // Emits an indirect call through the closure record `{ ptr fn, ptr captures, ptr
    // drop }` at `closurePtr`, allocating and threading the sret slot for a value return
    // and setting `outResolvedType`. A non-null `directFn` is called directly instead.
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
        // WHERE THE CALLED NAME IS WRITTEN, as distinct from where the call expression
        // starts: for `c.value()` the node begins at `c`, but the IDE's lookup is keyed
        // on `value`. Defaults to the node's own position for `super` and `this`.
        int nameLine = 0;
        int nameColumn = 0;

        // A compiler-built call with no parser context; the caller stamps the desugared
        // call site's span with setSourceSpan.
        MethodCallExpression(string name, vector<MethodCallParameter> params)
            : Expression(nullptr), methodCallName(std::move(name)),
              parameters(std::move(params)) { }
    private:
        // The runtime i64 title flag Cajeta.flagged(v, owned) pairs with this call's
        // value — how cajeta code mints "owned iff my bit says so". Null otherwise.
        llvm::Value* flaggedTitleValue = nullptr;
        // True for the `super(args)` alternative, which routes to the parent's
        // constructor: that form has no identifier context to dispatch on.
        bool superCtorCall = false;
        // Type args from `identifier<TypeArgs>(args)`; empty when they are inferred.
        vector<CajetaTypePtr> explicitMethodTypeArgs;
        // The un-projected wildcard sentinel behind `resolvedType`'s projected bound: an
        // outer call site compares it to detect a value from the same wildcard receiver.
        CajetaTypePtr preProjectionReturnType;
        // generateCode may run more than once, so this guards the bounded-reflection
        // rewrite that appends `Shape.class` as a synthesized second argument.
        bool boundedReflInjected = false;
        // Whether the resolved callee declares a `#T` return, so an enclosing site can
        // treat this result as a fresh owned temporary it must reclaim.
        bool resolvedReturnsOwnership = false;
        // Whether the line above is an ANSWER or merely its default. Set only where a
        // declared stance was determined, so a consumer can pick its own default rather
        // than read "nobody looked" as "borrow".
        bool resolvedReturnsOwnershipKnown = false;
        // The method this call resolved to, so consumers ask shape questions once.
        MethodPtr resolvedMethod;
        // Once this is set, a null `resolvedMethod` means an intrinsic lowering.
        bool codegenRan = false;
    public:
        bool isResolvedReturnsOwnership() const { return resolvedReturnsOwnership; }

        /** True when [isResolvedReturnsOwnership] reflects a real declaration. */
        bool hasKnownReturnStance() const { return resolvedReturnsOwnershipKnown; }

        /** "Does binding this call's result take a title?" Unknown answers OWNED: only
         *  intrinsic lowerings arrive unknown, and those allocate. A borrow-returning
         *  intrinsic would still over-claim here. */
        bool bindingTakesTitle() const {
            return resolvedReturnsOwnershipKnown ? resolvedReturnsOwnership : true;
        }
        llvm::Value* getFlaggedTitleValue() const { return flaggedTitleValue; }
        MethodPtr getResolvedMethod() const { return resolvedMethod; }
        /** True once generateCode has run (see codegenRan). */
        bool hasGenerated() const { return codegenRan; }

        // Statement-end temp classification, shared with the ctor-arg site: an anonymous
        // owned-String rvalue, and a shared-capable value result whose class is returned.
        static bool freshOwnedStringTemp(const AbstractSyntaxNodePtr& e);
        static shared_ptr<CajetaClass> freshSharedValueTempClass(
            const AbstractSyntaxNodePtr& e);
        // Temps whose title rides the transfer word: concrete vtable classes only, since
        // an interface has no drop entry that could take a title.
        static shared_ptr<CajetaClass> droppableTempClass(
            const CajetaTypePtr& t);
        // A plain `heap X(...)` creator, an anonymous owned rvalue that surrenders;
        // stack and shared placements never do.
        static shared_ptr<CajetaClass> freshHeapCreatorTempClass(
            const AbstractSyntaxNodePtr& e);
        // The array-typed twin; a stack or arena literal never surrenders, as the frame
        // reclaims its storage.
        static bool freshHeapArrayLiteralArg(const AbstractSyntaxNodePtr& e);
        CajetaTypePtr getPreProjectionReturnType() const {
            return preProjectionReturnType;
        }
        MethodCallExpression(CajetaParser::MethodCallContext* ctx, antlr4::Token* token);

        // Arguments are not in `children`, whose [0] is the receiver: a walk that must
        // see them (the lambda capture scan) comes through here.
        const vector<MethodCallParameter>& getParameters() const { return parameters; }

        // Visits the arguments too, which `children` does not hold.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            for (auto& p : parameters) {
                if (p.expression) fn(p.expression);
            }
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        const string& getMethodCallName() const { return methodCallName; }

        // Conservative callee peek for an argument-position call, classifying its return
        // stance before that argument generates. Unique name+arity match or null.
        static MethodPtr resolveArgCalleeShallow(
            const std::shared_ptr<MethodCallExpression>& call,
            CajetaModulePtr module);

        // The receiver half of the above, shared with xref so both apply the same rules.
        // `allowSuper` also resolves a `super.m()` receiver to the parent class.
        static shared_ptr<CajetaClass> resolveReceiverClassShallow(
            const std::shared_ptr<MethodCallExpression>& call,
            CajetaModulePtr module, bool allowSuper = false);
        void setMethodCallName(const string& name) { methodCallName = name; }
        bool isSuperCtorCall() const { return superCtorCall; }
        const vector<CajetaTypePtr>& getExplicitMethodTypeArgs() const {
            return explicitMethodTypeArgs;
        }

        // Walks the arguments, which `children` does not hold, and resolves the callee
        // under an open CallSiteScope so lint records a call edge. It must NOT cache
        // into `resolvedMethod`: that is codegen's full resolution, not this weaker one.
        void resolveTypes(CajetaModulePtr module) override;

    private:
        // Breaks a same-arity overload tie by the arguments' resolved types, unique-or-
        // nothing: an unresolved argument type disqualifies the attempt, never matches.
        MethodPtr resolveCalleeByArgTypes(CajetaModulePtr module);

        // Rebinds a generic method's type parameters into `declared` from this call
        // site's explicit type args, so a chain continues past `map<int64>(...)`.
        // Lint-only, and returns `declared` untouched for any shape that is not exact.
        CajetaTypePtr rebindMethodTypeArgs(const CajetaTypePtr& declared,
                                           const MethodPtr& callee);
    public:

        /** Resolves the callee and emits the call — intrinsic lowering, closure call,
         *  virtual dispatch or direct call — with the caller-side ownership work. */
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

} // code