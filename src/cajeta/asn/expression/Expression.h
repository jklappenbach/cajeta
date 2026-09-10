//
// Created by James Klappenbach on 3/19/22.
//

#pragma once

#include <list>
#include <string>
#include <cstdint>
#include <functional>
#include "../AbstractSyntaxNode.h"
#include "CajetaParser.h"
#include "../../type/CajetaType.h"

namespace llvm {
    class Function;
    class Value;
}

using namespace std;

/** The `expression` grammar production these node types cover; the full rule,
    with every alternative, lives in CajetaParser.g4. */

namespace cajeta {
    class CajetaModule;

    class CajetaType;

    class Field;

    class FormalParameter;

    class Block;

    class Annotation;

    class Expression;
    typedef shared_ptr<Expression> ExpressionPtr;

    // l-value to r-value coercion, shared by every expression-result consumer: it
    // loads through a slot pointer (an alloca, or a GEP for an element or field).
    llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v,
                              ExpressionPtr ast = nullptr);

    // Wraps a null-terminated `i8*` into a heap class String, mode-0 (owned), whose
    // `bytes` holds a COPY. `freeAfterWrap` frees the intermediate (not .rodata).
    llvm::Value* wrapCStringIntoClassString(CajetaModulePtr module,
        llvm::Value* cstr, const char* namePrefix,
        bool freeAfterWrap = true);

    // Rewrites a bare `[...]` against a class target as `heap Target([...])`, with
    // the element type from the target's first type argument. Null for a non-class.
    ExpressionPtr collectionLiteralFromArray(CajetaTypePtr target,
                                             const ExpressionPtr& literal);

    // The node-kind tag, one byte per node, so a consumer switches once instead of
    // chaining casts; the classifier's switch is -Werror=switch, so it stays total.
    enum class ExprKind : uint8_t {
        Unsupported = 0,
        Primary, Literal, ClassLiteral, This, Super,
        TextLiteral, IntegerLiteral, FloatLiteral,
        Identifier, Dot, ArrayIndex, ArraySlice, ArrayLiteral, MapLiteral,
        Aggregate, New, Cast, Postfix, Prefix, BinaryOp, BooleanSwitch,
        InstanceOf, MethodCall, Call, MethodReference, Move, Await, Spawn,
        Detach, Switch, Lambda,
        Count
    };

    // An expression in statement position is wrapped in ExpressionStatement.
    class Expression : public AbstractSyntaxNode {
    protected:
        bool primary;
        ExprKind exprKind = ExprKind::Unsupported;
        llvm::Value* titleFlagCache = nullptr;
        llvm::Function* titleFlagCacheFn = nullptr;
        // Set by the type-resolver, where the LLVM type alone is not enough.
        CajetaTypePtr resolvedType;
    public:
        Expression(antlr4::Token* token) : AbstractSyntaxNode(token) { }

        Expression(bool primary, antlr4::Token* token) : AbstractSyntaxNode(token) {
            this->primary = primary;
        }

        CajetaTypePtr getResolvedType() const { return resolvedType; }
        void setResolvedType(CajetaTypePtr t) { resolvedType = t; }
        /// The node-kind tag (see ExprKind), set by the subclass constructor.
        ExprKind kind() const { return exprKind; }

        /// 1.2.2 — the runtime title flag this node produced, cached per emitting function.
        llvm::Value* titleFlagCacheFor(llvm::Function* fn) const {
            return titleFlagCacheFn == fn ? titleFlagCache : nullptr;
        }
        void setTitleFlagCache(llvm::Value* v, llvm::Function* fn) {
            titleFlagCache = v;
            titleFlagCacheFn = fn;
        }

        virtual void addChild(ExpressionPtr expression) {
            children.push_back(expression);
        };

        static ExpressionPtr fromContext(CajetaParser::ExpressionContext* ctx);
    };

    /** primary: '(' expression ')' | THIS | SUPER | literal | identifier
        | typeTypeOrVoid '.' CLASS */
    class PrimaryExpression : public Expression {
    public:
        PrimaryExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Primary; }

        llvm::Value* generateCode(CajetaModulePtr module) override;

        static ExpressionPtr fromContext(CajetaParser::PrimaryContext* ctx);
    };

    // `T.class`: a primary whose value is the address of T's cached `#ClassObject`
    // and whose type is `Class<T>`. A primitive `T.class` has none and is rejected.
    class ClassLiteralExpression : public PrimaryExpression {
    public:
        ClassLiteralExpression(std::string namedTypeName, antlr4::Token* token)
            : PrimaryExpression(token),
              namedTypeName(std::move(namedTypeName)) { exprKind = ExprKind::ClassLiteral; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        // The type named before `.class`; the ANTLR context is freed by codegen.
        std::string namedTypeName;
        // Resolved lazily: the named type and its `Class<Foo>` instantiation.
        CajetaTypePtr namedType;
    };

    class ThisExpression : public PrimaryExpression {
    public:
        ThisExpression(CajetaParser::ExpressionContext* ctx) : PrimaryExpression(ctx->getStart()) { exprKind = ExprKind::This; }
        // For PrimaryExpression::fromContext: the THIS form has no ExpressionContext.
        ThisExpression(antlr4::Token* token) : PrimaryExpression(token) { exprKind = ExprKind::This; }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;

        // `this<Base>.field`: a `this` ADJUSTED to the named ancestor's sub-object,
        // with resolvedType set to it. Empty means plain `this`.
        const std::string& getChosenAncestorName() const { return chosenAncestorName; }
        void setChosenAncestorName(std::string name) { chosenAncestorName = std::move(name); }
    private:
        std::string chosenAncestorName;
    };

    // `super`: the same pointer as `this`, distinguished by resolvedType (the first
    // declared parent), so `super.foo()` skips the vtable. `super<Base>` picks one.
    class SuperExpression : public PrimaryExpression {
    public:
        SuperExpression(antlr4::Token* token) : PrimaryExpression(token) { exprKind = ExprKind::Super; }
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

    class ArrayIndexExpression : public Expression {
    public:
        ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Array/slice window `base[a:b]`, children = [base, from, to], yielding a
    // `Slice<E>` {store, off, len}; a slice base composes in O(1) against the ROOT.
    class ArraySliceExpression : public Expression {
    public:
        ArraySliceExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token);

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // List literal `[e1, e2, ...]`, elements in `children` in source order. Value
    // codegen is deferred - the launch path reads them off the AST.
    class ArrayLiteralExpression : public Expression {
    public:
        // Built from pre-parsed elements; the parse lives in arrayOrMapLiteralFromContext.
        ArrayLiteralExpression(vector<ExpressionPtr> elems, antlr4::Token* token);

        // Element expressions in source order (also mirrored into children).
        const vector<ExpressionPtr>& getElements() const { return elements; }

        // Push a target element type; set before resolveTypes it overrides unify.
        void setElementType(CajetaTypePtr t) { elementType = t; }

        // Placement: heap (default), stack (frame arena) or shared; at most one.
        void setStackAlloc(bool v) { stackAlloc = v; }
        void setSharedAlloc(bool v) { sharedAlloc = v; }
        bool isStackAlloc() const { return stackAlloc; }
        bool isSharedAlloc() const { return sharedAlloc; }

        // Set by computeArenaEligibility when a `stack [...]` proves non-escaping.
        void setArenaEligible(bool v) { arenaEligible = v; }
        bool isArenaEligible() const { return arenaEligible; }

        // spec 5.10 — the (slot, local) pairs this literal lends; filled by codegen, read by the binding declaration.
        const vector<std::pair<int, string>>& getBorrowedLocalSlots() const {
            return borrowedLocalSlots;
        }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        // Least-upper-bound of the element types; throws when there is none.
        CajetaTypePtr unifyElementType(CajetaModulePtr module);

        vector<ExpressionPtr> elements;
        vector<std::pair<int, string>> borrowedLocalSlots;
        CajetaTypePtr elementType;  // target (§3.2) or unified (§3.3)
        bool stackAlloc = false;    // `stack [...]` — frame arena (§4)
        bool sharedAlloc = false;   // `shared [...]` — device workgroup (§4)
        bool arenaEligible = false; // stack + proven non-escaping (§4)
    };

    // Map literal `[k1: v1, ...]` (and `[:]`), lowered to a `Pair<K,V>[]` passed to
    // `HashMap<K,V>(Pair<K,V>[])`. The parser routes here on any entry's `:`.
    class MapLiteralExpression : public Expression {
    public:
        MapLiteralExpression(vector<pair<ExpressionPtr, ExpressionPtr>> entries,
                             antlr4::Token* token);

        // Target map type pushed by context (a `HashMap<K,V>` instantiation).
        void setExpectedType(CajetaTypePtr t) { expectedType = std::move(t); }
        void setStackAlloc(bool v) { stackAlloc = v; }
        void setSharedAlloc(bool v) { sharedAlloc = v; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        vector<pair<ExpressionPtr, ExpressionPtr>> entries;
        CajetaTypePtr expectedType;  // pushed target `HashMap<K,V>` (§3.1)
        bool stackAlloc = false;
        bool sharedAlloc = false;
    };

    // Parses an `arrayLiteral` bracket list into a sequence or a map by the colon.
    ExpressionPtr arrayOrMapLiteralFromContext(
        CajetaParser::ArrayLiteralContext* ctx, antlr4::Token* token,
        bool stackAlloc, bool sharedAlloc);

    /** '(' annotation* typeType ('&' typeType)* ')' expression */
    class CastExpression : public Expression {
    private:
        CajetaTypePtr destType;
    public:
        CastExpression(CajetaTypePtr destType, antlr4::Token* token)
            : Expression(token), destType(destType) { exprKind = ExprKind::Cast; }

        // The cast's declared target type, available before resolveTypes runs (the
        // XPU device lowerer walks the kernel AST directly).
        CajetaTypePtr getDestType() const { return destType; }

        void resolveTypes(CajetaModulePtr module) override {
            AbstractSyntaxNode::resolveTypes(module);
            resolvedType = destType;
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** expression postfix=('++' | '--') */
    enum PostfixOp {
        POSTFIX_OP_INC, POSTFIX_OP_DEC
    };

    class PostfixExpression : public Expression {
        PostfixOp op;
    public:
        PostfixExpression(PostfixOp op, antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Postfix;
            this->op = op;
        }

        PostfixOp getOp() const { return op; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** prefix=('+'|'-'|'++'|'--'|'~'|'!') expression */
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
        PrefixExpression(PrefixOp op, antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Prefix;
            this->op = op;
        }

        PrefixOp getOp() const { return op; }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** Ternary `c ? a : b`; children = [cond, then, else]. Emits a conditional
     *  branch and a phi, the same shape as &&/||. */
    class BooleanSwitchExpression : public Expression {
    private:
        // The title flag of the arm actually TAKEN, an i64 in the merge block: 1 for
        // an arm that materialises a value, 0 for one that reads an existing one.
        llvm::Value* runtimeTitleFlag = nullptr;
    public:
        BooleanSwitchExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::BooleanSwitch; }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;

        llvm::Value* getRuntimeTitleFlag() const { return runtimeTitleFlag; }

        /// The leaf arms of a conditional or switch, through nesting, in source order: the conditional itself is transparent.
        template <class F>
        static void forEachLeafArm(const ExpressionPtr& e, F&& fn);
    };

    /** `expression instanceof (typeType | pattern)`. Compile-time only for now: a
     *  constant i1 from the lhs's resolvedType. A runtime check needs the
     *  class-hierarchy metadata StructureMetadata would emit. */
    class InstanceOfExpression : public Expression {
    private:
        CajetaTypePtr type;
        string pattern;
    public:
        InstanceOfExpression(CajetaTypePtr type, string pattern, antlr4::Token* token) : Expression(token) { exprKind = ExprKind::InstanceOf;
            this->type = type;
            this->pattern = pattern;
        }

        void resolveTypes(CajetaModulePtr module) override;

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    // Method reference `Type::method`, `obj::method` or `Type::heap`: a function-typed
    // value pointing at a thunk. Only the static form is implemented; the rest throw.
    class MethodReferenceExpression : public Expression {
    public:
        enum class Kind {
            STATIC,            // Type::staticMethod
            BOUND_INSTANCE,    // obj::method
            UNBOUND_INSTANCE,  // Type::instanceMethod
            CONSTRUCTOR        // Type::heap
        };
    private:
        // For `Type::id` / `Type::heap`: resolved eagerly at AST-build time.
        CajetaTypePtr receiverType;
        // For `obj::id`: the receiver expression; null for the type forms.
        ExpressionPtr receiverExpr;
        // Method name being referenced. Empty for CONSTRUCTOR.
        std::string methodName;
        bool isCtor;
        // Set by resolveTypes once the named method is known static or instance.
        Kind kind = Kind::STATIC;
        // Name of the synthesized thunk, generated lazily on first codegen.
        std::string thunkName;
        // Set when the ref captures its receiver by borrow, like LambdaExpression's
        // flag: the escape check at ReturnStatement consumes it.
        bool _hasBorrowCaptures = false;
        // Target-type hint from context: when the LHS is an sret-form function type
        // and the method's own ABI is borrow, this selects the sret adapter thunk.
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
              isCtor(isCtor) { exprKind = ExprKind::MethodReference;
            if (this->isCtor) kind = Kind::CONSTRUCTOR;
        }

        // For the device kernel lowerer, which walks the AST and needs the eagerly
        // resolved receiver type and method name without running resolveTypes.
        CajetaTypePtr getReceiverType() const { return receiverType; }
        ExpressionPtr getReceiverExpr() const { return receiverExpr; }
        const std::string& getMethodName() const { return methodName; }
        bool getIsCtor() const { return isCtor; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** `#expr`, the move/transfer operator (MemoryModel.md). Codegen delegates to
     *  its child and marks an identifier child moved in the active scope; the
     *  transfer flow itself lives at the use site (assign, argument, return). */
    class MoveExpression : public Expression {
    private:
        // When the moved-out source is a runtime owner, its entry flag is captured
        // here BEFORE deactivation; store and return sites seed their bit from it.
        llvm::Value* runtimeTitleFlag = nullptr;
    public:
        MoveExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Move; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
        llvm::Value* getRuntimeTitleFlag() const { return runtimeTitleFlag; }
        // `#= #x`, the transfer spelled twice: it means what `#= x` means, so it
        // warns rather than rejects, and generateCode reports it.
        void setRedundantSharp(bool v) { redundantSharp = v; }
        bool isRedundantSharp() const { return redundantSharp; }

        // Fused slot-to-slot forwarding, set by the enclosing `dst[i] #= #src[j]`
        // store: the extraction forwards the source bit instead of claiming title.
        void setForwardingSlotMove(bool v) { forwardingSlotMove = v; }
        bool isForwardingSlotMove() const { return forwardingSlotMove; }
        // Set when the move was spelled `dst = #v`. It cannot be recovered later:
        // `dst = #v` and `dst #= v` build the SAME node.
        void setLegacyTransferAssign(bool v) { legacyTransferAssign = v; }
        bool isLegacyTransferAssign() const { return legacyTransferAssign; }

        // Set on the wrapper the `dst #= v` desugar synthesises. `#=` is MODE-CARRYING,
        // so it is exempt from the transfer-of-a-borrow rejection of a FALSE claim.
        void setModeCarrying(bool v) { modeCarrying = v; }
        bool isModeCarrying() const { return modeCarrying; }

        // Set by EVERY `#=` spelling, assignment and declaration alike: it marks
        // that the store records the source's mode. Deliberately not `modeCarrying`,
        // which would also exempt a declaration from the provenance rejections.
        void setSharpStore(bool v) { sharpStore = v; }
        bool isSharpStore() const { return sharpStore; }
    private:
        bool forwardingSlotMove = false;
        bool redundantSharp = false;
        bool legacyTransferAssign = false;
        bool modeCarrying = false;
        bool sharpStore = false;
    };

    // Structured-concurrency expressions, each wrapping one inner expression in
    // children[0]: await takes a Task<T> and yields T, spawn runs a call and packs
    // a completed Task<T>, and detach is spawn with the Task discarded.
    class AwaitExpression : public Expression {
    public:
        AwaitExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Await; }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    class SpawnExpression : public Expression {
    private:
        // Drop-chain entry for the malloced Task, consumed by assignment sites to
        // mark it inactive once ownership passes to the bound local's own entry.
        llvm::Value* dropEntry = nullptr;
        // Detach skips scope_register and drop_push: no scope owns the Task, and
        // its heap allocation lives for the process.
        bool detachMode = false;
        // Statement-position spawn whose Task is never bound. It registers with the
        // scope frame as SCOPE-OWNED instead of taking a drop entry: one per-site
        // entry cannot represent N live tasks from a loop.
        bool discardedMode = false;
    public:
        SpawnExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Spawn; }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
        llvm::Value* getDropEntry() const { return dropEntry; }
        void setDetachMode(bool v) { detachMode = v; }
        bool getDetachMode() const { return detachMode; }
        void setDiscardedMode(bool v) { discardedMode = v; }
        bool getDiscardedMode() const { return discardedMode; }
    };

    class DetachExpression : public Expression {
    public:
        DetachExpression(antlr4::Token* token) : Expression(token) { exprKind = ExprKind::Detach; }
        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    /** Java-17 switch expression, arrow form with single-expression case bodies
     *  only (`case 1, 2 -> 10;`), which avoids needing `yield`. The cases lower to
     *  an llvm::SwitchInst, with a phi collecting each arm's value. */
    class SwitchExpression : public Expression {
    public:
        struct Case {
            list<ExpressionPtr> labels;     // empty == default
            ExpressionPtr body;
        };
    private:
        ExpressionPtr discriminator;
        list<Case> cases;
        // Unit 2 — the title flag of the arm TAKEN, an i64 in the merge block; null for a non-pointer result.
        llvm::Value* runtimeTitleFlag = nullptr;
    public:
        const list<Case>& getCases() const { return cases; }
        llvm::Value* getRuntimeTitleFlag() const { return runtimeTitleFlag; }
        SwitchExpression(antlr4::Token* token,
                          ExpressionPtr discriminator,
                          list<Case> cases)
            : Expression(token),
              discriminator(std::move(discriminator)),
              cases(std::move(cases)) { exprKind = ExprKind::Switch; }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    };

    // Non-capturing lambda `(int32 a, int32 b) -> a + b`, lowered to a synthesized
    // static function whose address the expression evaluates to. v1 takes explicit
    // parameter types and expression bodies only, with no captures.
    class LambdaExpression : public Expression {
    private:
        std::vector<std::string> paramNames;
        std::vector<CajetaTypePtr> paramTypes;
        // An Expression for the expression-body form, or a Block; codegen
        // dispatches on the dynamic type.
        AbstractSyntaxNodePtr body;
        // Name of the synthesized function, unique per module, set on first codegen.
        std::string synthesizedName;
        // True once capture analysis found a borrow capture; the escape check gates
        // return-of-closure and store-to-non-local on it.
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
              body(std::move(body)) { exprKind = ExprKind::Lambda; }

        const std::vector<std::string>& getParamNames() const { return paramNames; }
        const std::vector<CajetaTypePtr>& getParamTypes() const { return paramTypes; }
        AbstractSyntaxNodePtr getBody() const { return body; }

        // 7.2.4 — the body lives in a private slot, not `children`.
        void forEachSubNode(
                const std::function<void(const AbstractSyntaxNodePtr&)>& fn) override {
            if (body) fn(body);
            AbstractSyntaxNode::forEachSubNode(fn);
        }

        // Target-type hint from context; codegen uses it as the lambda's
        // CajetaFunctionType, since a body's return type is not inferred yet.
        void setExpectedType(CajetaTypePtr t) { expectedType = std::move(t); }

        void resolveTypes(CajetaModulePtr module) override;
        llvm::Value* generateCode(CajetaModulePtr module) override;
    private:
        CajetaTypePtr expectedType;
    };

    // Placeholder for grammar forms not yet implemented: generateCode throws a
    // cajeta::Exception naming the construct and its source location.
    class UnsupportedExpression : public Expression {
    private:
        string constructName;
    public:
        UnsupportedExpression(string constructName, antlr4::Token* token)
            : Expression(token), constructName(std::move(constructName)) { exprKind = ExprKind::Unsupported; }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };


    // True when the right-hand side of a `#=` carries its own `#`. The store already
    // IS the transfer, so the second sharp is redundant; defined in Expression.cpp.
    bool cajetaRhsCarriesRedundantSharp(
        CajetaParser::ExpressionContext* rhs);

    // The leaf-arm walk over both conditional kinds; out of the class because it needs SwitchExpression complete.
    template <class F>
    void BooleanSwitchExpression::forEachLeafArm(const ExpressionPtr& e, F&& fn) {
        if (!e) return;
        if (e->kind() == ExprKind::BooleanSwitch) {
            auto& ch = e->getChildren();
            for (size_t i = 1; i < ch.size() && i < 3; ++i) {
                forEachLeafArm(dynamic_pointer_cast<Expression>(ch[i]), fn);
            }
            return;
        }
        if (e->kind() == ExprKind::Switch) {
            for (auto& c : static_cast<SwitchExpression*>(e.get())->getCases()) {
                if (c.body) forEachLeafArm(c.body, fn);
            }
            return;
        }
        fn(e);
    }

    /// True for a conditional of either kind (`c ? a : b`, `switch (x) {…}`).
    inline bool isConditionalKind(const ExpressionPtr& e) {
        return e && (e->kind() == ExprKind::BooleanSwitch || e->kind() == ExprKind::Switch);
    }

    /// True for the `#x` / `#=` wrapper — a SPELLING test; what the move carries is ownership::classify's question.
    inline bool isMoveKind(const AbstractSyntaxNodePtr& n) {
        auto e = std::dynamic_pointer_cast<Expression>(n);
        return e && e->kind() == ExprKind::Move;
    }

    /// The operand of a `#x` / `#=` wrapper, or null when `n` is not one.
    inline ExpressionPtr moveInner(const AbstractSyntaxNodePtr& n) {
        if (!isMoveKind(n)) return nullptr;
        auto& kids = n->getChildren();
        return kids.empty() ? nullptr : std::dynamic_pointer_cast<Expression>(kids[0]);
    }

    /// The title flag a conditional computed for its taken arm; null for a non-conditional or non-pointer result.
    inline llvm::Value* conditionalTitleFlag(const ExpressionPtr& e) {
        if (!e) return nullptr;
        if (e->kind() == ExprKind::BooleanSwitch) {
            return static_cast<BooleanSwitchExpression*>(e.get())->getRuntimeTitleFlag();
        }
        if (e->kind() == ExprKind::Switch) {
            return static_cast<SwitchExpression*>(e.get())->getRuntimeTitleFlag();
        }
        return nullptr;
    }
}
