//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <list>
#include <string>
#include "../AbstractSyntaxNode.h"
#include "CajetaParser.h"
#include "../../type/CajetaType.h"

using namespace std;

/**
expression
    : primary
    | REFERENCE primary
    | expression bop='.'
      (
         identifier
       | methodCall
       | THIS
       | NEW nonWildcardTypeArguments? innerCreator
       | SUPER superSuffix
      )
    | expression '[' expression ']'
    | methodCall
    | NEW creator
    | '(' annotation* typeType ('&' typeType)* ')' expression
    | expression postfix=('++' | '--')
    | prefix=('+'|'-'|'++'|'--') expression
    | prefix=('~'|'!') expression
    | expression bop=('*'|'/'|'%') expression
    | expression bop=('+'|'-') expression
    | expression ('<' '<' | '>' '>' '>' | '>' '>') expression
    | expression bop=('<=' | '>=' | '>' | '<') expression
    | expression bop=INSTANCEOF (typeType | pattern)
    | expression bop=('==' | '!=') expression
    | expression bop='&' expression
    | expression bop='^' expression
    | expression bop='|' expression
    | expression bop='&&' expression
    | expression bop='||' expression
    | <assoc=right> expression bop='?' expression ':' expression
    | <assoc=right> expression
      bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=')
      expression
    | lambdaExpression // Java8
    | switchExpression // Java17

    // Java 8 methodReference
    | expression '::' typeArguments? identifier
    | typeType '::' (typeArguments? identifier | NEW)
    | classType '::' typeArguments? NEW
    ;
*/

namespace cajeta {
    class CajetaModule;

    class CajetaType;

    class Field;

    class FormalParameter;

    class Block;

    class Annotation;

    class Expression;
    typedef shared_ptr<Expression> ExpressionPtr;

    // l-value → r-value coercion shared between expression-result consumers
    // (BinaryOp's RHS/LHS load paths, ReturnStatement, LambdaExpression's
    // body, VariableInitializer). Given a value `v` that may be a pointer
    // to a slot (alloca for locals, GEP for array elements / struct
    // fields), loads through to produce a value of the wrapped type.
    // Scalars and intermediate r-values pass through unchanged. `ast` is
    // optional; when provided it lets the helper distinguish ArrayIndex
    // and DotExpression slot pointers (which need element/field-type-
    // driven loads) from anonymous pointer values.
    llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v,
                              ExpressionPtr ast = nullptr);

    // Wrap a C-string `cstr` (an `i8*` pointing at malloc'd bytes that
    // ARE null-terminated) into a heap-allocated class String instance.
    // The String is mode-0 (owned) — its `bytes` field points at a
    // fresh CajetaArray header that holds a copy of the bytes. When
    // `freeAfterWrap` is true the intermediate cstr is freed at the
    // call site (use false for static literals / .rodata pointers).
    //
    // Shared between MethodCallExpression (toString/valueOf intrinsics)
    // and DotExpression (view-field String materialization).
    llvm::Value* wrapCStringIntoClassString(CajetaModulePtr module,
        llvm::Value* cstr, const char* namePrefix,
        bool freeAfterWrap = true);

    // Expression is a sibling of Statement under AbstractSyntaxNode. When an expression
    // appears in statement position (e.g. `foo();`), wrap it in ExpressionStatement
    // rather than relying on inheritance — see Statement::fromContext.
    class Expression : public AbstractSyntaxNode {
    protected:
        bool primary;
        // Set by the type-resolver pass; codegen consults this for situations where the
        // LLVM type alone is insufficient (e.g. fp8 stored as i8). May be null pre-resolution.
        CajetaTypePtr resolvedType;
    public:
        Expression(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        Expression(bool primary, antlr4::Token* token) : AbstractSyntaxNode(token) {
            this->primary = primary;
        }

        CajetaTypePtr getResolvedType() const { return resolvedType; }
        void setResolvedType(CajetaTypePtr t) { resolvedType = t; }

        virtual void addChild(ExpressionPtr expression) {
            children.push_back(expression);
        };

        static ExpressionPtr fromContext(CajetaParser::ExpressionContext* ctx);
    };

    /**
        : '(' expression ')'
        | THIS
        | SUPER
        | literal
        | identifier
        | typeTypeOrVoid '.' CLASS
        ;
     */
    class PrimaryExpression : public Expression {
    public:
        PrimaryExpression(antlr4::Token* token) : Expression(token) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;

        static ExpressionPtr fromContext(CajetaParser::PrimaryContext* ctx);
    };

    // REFL-1.5: `T.class` literal. A primary expression
    // (`typeTypeOrVoid '.' CLASS`) whose value is the address of T's cached
    // `#ClassObject` and whose type is `Class<T>` (the statically-known type's
    // reflective Class — the static counterpart to `obj.getClass()` /
    // `Class.of(obj)`). v1 supports class types; a primitive `T.class` has no
    // `#ClassObject` and is rejected.
    class ClassLiteralExpression : public PrimaryExpression {
    public:
        ClassLiteralExpression(std::string namedTypeName, antlr4::Token* token)
            : PrimaryExpression(token),
              namedTypeName(std::move(namedTypeName)) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        // The text of the type named before `.class` (e.g. `Foo`), captured at
        // parse time — the ANTLR context is freed before codegen, so we cannot
        // hold the TypeTypeOrVoidContext and call fromContext on it later.
        std::string namedTypeName;
        // Resolved lazily in resolveTypes / generateCode: the named type's
        // CajetaClass (looked up by name in canonicalMap), and the
        // `Class<Foo>` instantiation.
        CajetaTypePtr namedType;
    };

    class ThisExpression : public PrimaryExpression {
    public:
        ThisExpression(CajetaParser::ExpressionContext* ctx) : PrimaryExpression(ctx->getStart()) { }
        // Used by PrimaryExpression::fromContext, where `ctx` is a
        // PrimaryContext (not an ExpressionContext). Previously the
        // call site passed `ctx->expression()` which is null for the
        // THIS form — the result was a null-deref the moment ThisExpression
        // tried to read getStart(). Now we accept the Token directly.
        ThisExpression(antlr4::Token* token) : PrimaryExpression(token) { }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;

        // MultiClassing Phase 2 (P-2): `this<Base>.field` — primary
        // expression that resolves to a `this` pointer ADJUSTED to the
        // selected ancestor's sub-object. resolvedType becomes the
        // chosen ancestor (not the current class), so DotExpression's
        // field lookup uses `Base`'s propertyList directly (skipping the
        // sibling-collision walk that fires for unqualified `this.field`).
        // Set via the visitor when `THIS '[' typeType ']'` parses; empty
        // string means plain `this` (no adjustment).
        const std::string& getChosenAncestorName() const { return chosenAncestorName; }
        void setChosenAncestorName(std::string name) { chosenAncestorName = std::move(name); }
    private:
        std::string chosenAncestorName;
    };

    // `super` as a primary expression. Value is the same pointer as
    // `this` (a super-reference doesn't physically point to a different
    // object); the distinction is in resolvedType — it's the current
    // class's first declared parent — so a `super.foo()` call lands
    // on the PARENT's method via direct dispatch (bypassing the
    // instance's vtable, which would loop back to the override).
    //
    // MultiClassing Phase 2 adds the bracketed variant `super<Base>`
    // which selects an explicit ancestor (any one — first parent,
    // sibling parent, or further ancestor). Plain `super` resolves to
    // the first declared parent for back-compat. The chosen-ancestor
    // value flows through resolveTypes (set as resolvedType) and
    // generateCode (`this` is adjusted to the ancestor sub-object via
    // CajetaClass::adjustForUpcast before the call site reads it).
    class SuperExpression : public PrimaryExpression {
    public:
        SuperExpression(antlr4::Token* token) : PrimaryExpression(token) { }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;

        const std::string& getChosenAncestorName() const { return chosenAncestorName; }
        void setChosenAncestorName(std::string name) { chosenAncestorName = std::move(name); }
    private:
        std::string chosenAncestorName;
    };

    enum ReservedIdentifiers {
        UNKNOWN = -1,
        MODULE,
        REQUIRE,
        EXPORTS,
        OPENS,
        TO,
        USES,
        PROVIDES,
        WITH,
        TRANSITIVE,
        YIELD,
        SEALED,
        PERMITS,
        RECORD,
        VAR
    };

    // Removed dead classes: ClassExpression, GenericsExpression, BopExpression and its 6
    // subclasses, ParExpression. Their grammar productions are handled by DotExpression
    // or are unreached. Member access lowers via DotExpression directly.

    class ArrayIndexExpression : public Expression {
    public:
        ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Array/slice window: `base[a:b]` (slice-spec §2/§7.2; slices plan Unit 7).
    // children = [base, from, to]. Yields a `Slice<E>` VALUE (an alloca of the
    // 3-word {store, off, len} body):
    //   - array base:  {arr, a, b-a} windowing the root (from/to clamped to
    //     [0, count] like String.substring);
    //   - slice base:  {base.store, base.off + a, b-a} — O(1) composition
    //     attributing to the ROOT.
    class ArraySliceExpression : public Expression {
    public:
        ArraySliceExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // List literal: `[e1, e2, ...]` (and the empty `[]`). Grammar:
    // `arrayLiteral : '[' expressionList? ']'`, reachable from `primary`
    // (CajetaParser.g4). Introduced for the XPU launch dimensions
    // (`grid: [(n+255)/256]`, `block: [256]`) per CajetaXPU.md §3.1.3, but
    // general-purpose. Each element expression is held in `children` in
    // source order. Value codegen is deferred — the launch path reads the
    // element expressions directly off the AST — so generateCode rejects
    // standalone use for now.
    class ArrayLiteralExpression : public Expression {
    public:
        ArrayLiteralExpression(CajetaParser::ArrayLiteralContext* ctx, antlr4::Token* token);

        // Element expressions in source order (also mirrored into children).
        const vector<ExpressionPtr>& getElements() const { return elements; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        vector<ExpressionPtr> elements;
    };


    /**
     * '(' annotation* typeType ('&' typeType)* ')' expression
     */
    class CastExpression : public Expression {
    private:
        CajetaTypePtr destType;
    public:
        CastExpression(CajetaTypePtr destType, antlr4::Token* token)
            : Expression(token), destType(destType) { }

        // The cast's declared target type. Available even before resolveTypes()
        // runs (the XPU device lowerer walks the kernel AST directly and may
        // not have populated resolvedType).
        CajetaTypePtr getDestType() const { return destType; }

        void resolveTypes(CajetaModulePtr module) override {
            AbstractSyntaxNode::resolveTypes(module);
            resolvedType = destType;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * expression postfix=('++' | '--')
     */
    enum PostfixOp {
        POSTFIX_OP_INC, POSTFIX_OP_DEC
    };

    class PostfixExpression : public Expression {
        PostfixOp op;
    public:
        PostfixExpression(PostfixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }

        PostfixOp getOp() const { return op; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * prefix=('+'|'-'|'++'|'--'|'~'|'!') expression
     */
    enum PrefixOp {
        PREFIX_OP_POSITIVE,
        PREFIX_OP_NEGATIVE,
        PREFIX_OP_INC,
        PREFIX_OP_DEC,
        PREFIX_OP_BITNOT,   // ~
        PREFIX_OP_LOGNOT    // !
    };

    class PrefixExpression : public Expression {
    private:
        PrefixOp op;
    public:
        PrefixExpression(PrefixOp op, antlr4::Token* token) : Expression(token) {
            this->op = op;
        }

        PrefixOp getOp() const { return op; }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Removed dead classes: LogicalPrefixExpression, BitshiftExpression, ComparisonExpression,
    // EquivalenceExpression, BitwiseAndExpression, BitwiseExOrExpression,
    // BitwiseInclusiveOrExpression, LogicalAndExpression, LogicalOrExpression,
    // ArithmeticAssignmentExpression. All of these are handled by BinaryOpExpression
    // variants now (shift / comparison / equality / bitwise / logical / *_EQUALS).
    // PrefixExpression handles ~ / ! / +/- / ++/--.

    /**
     * <assoc=right> expression bop='?' expression ':' expression
     *
     * Ternary. children = [cond, then, else] from fromContext's child-population loop.
     * Emits a conditional branch + phi pattern, same shape as &&/||.
     */
    class BooleanSwitchExpression : public Expression {
    public:
        BooleanSwitchExpression(antlr4::Token* token) : Expression(token) { }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * expression bop=INSTANCEOF (typeType | pattern)
     *
     * Compile-time only for now: emits a constant i1 by comparing the lhs's
     * resolvedType to the target type. A real runtime check needs class-hierarchy
     * metadata emitted via StructureMetadata (so we can walk parents at runtime);
     * that's tracked separately.
     */
    class InstanceOfExpression : public Expression {
    private:
        CajetaTypePtr type;
        string pattern;
    public:
        InstanceOfExpression(CajetaTypePtr type, string pattern, antlr4::Token* token) : Expression(token) {
            this->type = type;
            this->pattern = pattern;
        }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    // Method reference: `Type::method`, `obj::method`, or `Type::heap`.
    // Compiles to a function-typed value (same value-level shape as a
    // lambda) pointing at a synthesized thunk that adapts the underlying
    // method to the closure ABI (`ptr captures` as the first arg).
    // L4-1 implements only the static-method form
    // (`Type::staticMethod`); bound instance refs, unbound instance
    // refs, and constructor refs throw NOT_IMPLEMENTED until the
    // matching sub-slices land. See docs/specification/lang/Lambdas.md § Method
    // references.
    class MethodReferenceExpression : public Expression {
    public:
        enum class Kind {
            STATIC,            // Type::staticMethod
            BOUND_INSTANCE,    // obj::method
            UNBOUND_INSTANCE,  // Type::instanceMethod
            CONSTRUCTOR        // Type::heap
        };
    private:
        // For `Type::id` / `Type::heap`: the type appears literally in
        // the source as a typeType and we resolve it eagerly at AST
        // build time (its name is in scope unconditionally).
        CajetaTypePtr receiverType;
        // For `obj::id`: the receiver is a runtime expression evaluated
        // at the reference site. nullptr for the type-receiver forms.
        ExpressionPtr receiverExpr;
        // Method name being referenced. Empty for CONSTRUCTOR.
        std::string methodName;
        bool isCtor;
        // Set by resolveTypes once we know whether the named method is
        // static or instance. STATIC for either form when the target
        // method is static; BOUND_INSTANCE for obj::method on an
        // instance method; UNBOUND_INSTANCE for Type::method on an
        // instance method.
        Kind kind = Kind::STATIC;
        // Name of the synthesized thunk function, generated lazily on
        // first codegen.
        std::string thunkName;
        // Set when this method ref captures the receiver by borrow
        // (kind == BOUND_INSTANCE) — analogous to LambdaExpression's
        // hasBorrowCaptures flag, consumed by the L3-2 escape check at
        // ReturnStatement and propagated to function-typed Fields.
        bool _hasBorrowCaptures = false;
        // Target-type hint from the surrounding context, mirroring
        // LambdaExpression. When the LHS is an sret-form function-type and
        // the underlying method's natural ABI is borrow (non-sret, non-#),
        // setting expectedType lets resolveTypes pick the sret-form fnType
        // and generateCode synthesize the borrow→sret adapter thunk
        // (docs/specification/lang/ValueReturns.md — M5(b) adapter).
        CajetaTypePtr expectedType;
    public:
        bool getHasBorrowCaptures() const { return _hasBorrowCaptures; }
        void setExpectedType(CajetaTypePtr t) { expectedType = std::move(t); }
        MethodReferenceExpression(antlr4::Token* token,
                                  CajetaTypePtr receiverType,
                                  ExpressionPtr receiverExpr,
                                  std::string methodName,
                                  bool isCtor)
            : Expression(token),
              receiverType(std::move(receiverType)),
              receiverExpr(std::move(receiverExpr)),
              methodName(std::move(methodName)),
              isCtor(isCtor) {
            if (this->isCtor) kind = Kind::CONSTRUCTOR;
        }

        // Accessors used by the device kernel lowerer (Stage 11 bounded
        // dispatch), which walks the AST directly and needs the eagerly-
        // resolved receiver type + method name without running resolveTypes.
        CajetaTypePtr getReceiverType() const { return receiverType; }
        ExpressionPtr getReceiverExpr() const { return receiverExpr; }
        const std::string& getMethodName() const { return methodName; }
        bool getIsCtor() const { return isCtor; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Placeholder for expression forms recognized by the grammar but not yet implemented
    // (lambdas, switch expressions, super dispatch, inner-class new, method references).
    // The parser produces one of these so the failure surfaces at codegen time as a clear
    // cajeta::Exception with the construct name and source location, rather than a silent
    /**
     * `#expr` — the move/transfer operator. See `MemoryModel.md` for full
     * semantics. At codegen time:
     *   1. Delegates value-generation to its single child.
     *   2. If that child is an `IdentifierExpression`, marks the identifier as
     *      moved in the active scope so subsequent reads can be rejected.
     *   3. Carries a static-checked invariant: nested `##expr` reports the
     *      same use-after-move error pattern after the first wrap unwinds.
     *
     * The transfer-flow side (drop-elision on the source, ownership transfer
     * to the destination) lives at the use site (`BinaryOpExpression` for
     * assignment, `MethodCallExpression` for arguments, `ReturnStatement` for
     * returns). They detect that their child is a `MoveExpression` via
     * `dynamic_pointer_cast` and act accordingly.
     */
    class MoveExpression : public Expression {
    private:
        // 5.2.2 — when the moved-out source is a runtime owner (a formal
        // whose title is a transfer-word bit), its entry flag is captured
        // here BEFORE deactivation; store/return sites seed their bit from
        // it. Null for static owners (compile-time truth stands).
        llvm::Value* runtimeTitleFlag = nullptr;
    public:
        MoveExpression(antlr4::Token* token) : Expression(token) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
        llvm::Value* getRuntimeTitleFlag() const { return runtimeTitleFlag; }
    };

    // Structured-concurrency expressions (docs/specification/concurrent/Concurrency.md). All three wrap a
    // single inner expression in children[0]. In the sync-lowering MVP:
    //   - AwaitExpression: takes a Task<T> value, returns the inner T.
    //   - SpawnExpression: takes a method-call invocation, runs it immediately,
    //     packs the result in a fresh completed Task<T>.
    //   - DetachExpression: same as spawn but discards the Task. The
    //     "captures must be #-transferred" rule lands with the real scheduler.
    // Real scheduling, suspension at await, and scope-bound child lifetimes
    // come in later phases; today these are straight-line lowerings.
    class AwaitExpression : public Expression {
    public:
        AwaitExpression(antlr4::Token* token) : Expression(token) { }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class SpawnExpression : public Expression {
    private:
        // Drop-chain entry alloca for the malloced Task. Populated during
        // generateCode after __cajeta_drop_push runs. Consumed by
        // assignment sites (LocalVariableDeclaration's initializer path
        // and BinaryOpExpression ASSIGN) to call __cajeta_drop_mark_inactive
        // when the spawn result is bound to a named owner — ownership
        // transfers to the local's own class-instance drop entry, so the
        // spawn's transient entry shouldn't fire. See AsyncStatus.md §
        // Plan: Task<T> as user-typeable template / Ownership-transfer.
        llvm::Value* dropEntry = nullptr;
        // Detach mode skips scope_register (so scope_exit won't wait for
        // this task) and skips drop_push (no scope owns the Task; its
        // heap allocation leaks for the process lifetime per
        // docs/specification/concurrent/Concurrency.md § detach semantics). Set by DetachExpression
        // before calling generateCode through this same trampoline path
        // — single source of truth for the spawn/detach lowering.
        bool detachMode = false;
    public:
        SpawnExpression(antlr4::Token* token) : Expression(token) { }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
        llvm::Value* getDropEntry() const { return dropEntry; }
        void setDetachMode(bool v) { detachMode = v; }
        bool getDetachMode() const { return detachMode; }
    };

    class DetachExpression : public Expression {
    public:
        DetachExpression(antlr4::Token* token) : Expression(token) { }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /**
     * Java-17 switch expression. Today we only support the arrow form with single-
     * expression case bodies — `case X -> expr;` and `default -> expr;` — which is
     * the most common shape and avoids needing `yield`.
     *
     *   switch (x) {
     *     case 1, 2 -> 10;
     *     case 3    -> 20;
     *     default   -> 99;
     *   }
     *
     * Each case may match multiple constants via a comma-separated list. The cases
     * lower to an `llvm::SwitchInst` and a phi node collects each arm's value.
     */
    class SwitchExpression : public Expression {
    public:
        struct Case {
            list<ExpressionPtr> labels;     // empty == default
            ExpressionPtr body;
        };
    private:
        ExpressionPtr discriminator;
        list<Case> cases;
    public:
        SwitchExpression(antlr4::Token* token,
                          ExpressionPtr discriminator,
                          list<Case> cases)
            : Expression(token),
              discriminator(std::move(discriminator)),
              cases(std::move(cases)) { }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Non-capturing lambda: `(int32 a, int32 b) -> a + b`. Lowers at codegen
    // to a synthesized static LLVM function whose body is the lambda's
    // expression; the lambda expression itself evaluates to that function's
    // address (a `ptr`), which a CajetaFunctionType-typed slot can hold.
    //
    // v1 (L1) limits:
    //  - Explicit parameter types required. Elided forms (`(a, b) -> ...`)
    //    that rely on target-type inference from the surrounding type
    //    context are deferred to L1.5 or later.
    //  - Expression body only — no block bodies yet.
    //  - No captures. Bodies that reference outer-scope names produce a
    //    codegen error; capture support arrives in L2.
    class LambdaExpression : public Expression {
    private:
        std::vector<std::string> paramNames;
        std::vector<CajetaTypePtr> paramTypes;
        // L1/L1.5: an Expression (expression-body form, e.g. `x -> x + 1`).
        // L2-4 onwards: may also be a Block (`{ stmt; return val; }`),
        // hence the AbstractSyntaxNode-typed slot. Codegen dispatches
        // based on the dynamic type.
        AbstractSyntaxNodePtr body;
        // Name of the synthesized LLVM function. Unique per module via a
        // monotonic counter; set lazily on first codegen.
        std::string synthesizedName;
        // True once capture analysis has run AND found at least one
        // borrow capture (heap reference, not primitive copy, not `#`
        // transfer). Read after generateCode by the escape check that
        // gates return-of-closure / store-to-non-local. Closures with
        // only by-value or by-transfer captures are free to escape;
        // borrow captures are bounded by the declaring scope.
        bool hasBorrowCaptures = false;
    public:
        bool getHasBorrowCaptures() const { return hasBorrowCaptures; }
        LambdaExpression(antlr4::Token* token,
            std::vector<std::string> paramNames,
            std::vector<CajetaTypePtr> paramTypes,
            AbstractSyntaxNodePtr body)
            : Expression(token),
              paramNames(std::move(paramNames)),
              paramTypes(std::move(paramTypes)),
              body(std::move(body)) { }

        const std::vector<std::string>& getParamNames() const { return paramNames; }
        const std::vector<CajetaTypePtr>& getParamTypes() const { return paramTypes; }
        AbstractSyntaxNodePtr getBody() const { return body; }

        // Target-type hint from the surrounding context (e.g. a LHS
        // function-typed declaration). When set, codegen uses this as the
        // lambda's CajetaFunctionType — its return type is what the lambda's
        // synthesized function returns. Without it, codegen would have to
        // infer return type from the body, which requires every body-shape
        // expression to populate its own resolvedType — not the case yet.
        void setExpectedType(CajetaTypePtr t) { expectedType = std::move(t); }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        CajetaTypePtr expectedType;
    };

    // nullptr returning invalid IR.
    class UnsupportedExpression : public Expression {
    private:
        string constructName;
    public:
        UnsupportedExpression(string constructName, antlr4::Token* token)
            : Expression(token), constructName(std::move(constructName)) { }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

}
