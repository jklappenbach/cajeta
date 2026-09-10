//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "../type/Modifiable.h"
#include "llvm/IR/BasicBlock.h"
#include "../type/QualifiedName.h"
#include "../type/Annotatable.h"
#include "../type/CajetaType.h"
#include "../type/FormalParameter.h"
#include "../asn/Block.h"
#include "../type/Scope.h"
#include "../type/CajetaType.h"
#include "../field/Field.h"
#include "../type/Templates.h"
#include "../prof/ProfileFrame.h"
#include "queue"
#include "map"
#include "unordered_map"

using namespace std;

namespace cajeta {
    class CajetaModule;

    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Expression;

    class CajetaClass;

    // A method's LLVMContext-bound codegen bindings; a frozen (shared) parent
    // class moves them into the per-thread side table frozenMethodBindings().
    struct MethodLlvmBindings {
        llvm::FunctionType* llvmFunctionType = nullptr;
        llvm::Function* llvmFunction = nullptr;
        llvm::Function* llvmOriginalFunction = nullptr;
    };

    typedef shared_ptr<CajetaClass> CajetaClassPtr;

    class Mathod;

    typedef shared_ptr<Method> MethodPtr;

    // Which `@Before` / `@After` / etc. annotation sits on the advice method.
    enum class AdviceKind {
        Before,
        After,
        Around,
        AfterReturning,
        AfterThrowing,
    };

    // How the advice's pointcut argument resolved: a marker annotation (every
    // method carrying it) or a type (every method of that class or a subclass).
    enum class PointcutShape {
        MarkerAnnotation,
        Type,
    };

    // One (aspect, advice method, kind, pointcut shape) match on a user method,
    // cached by the pointcut pass for codegen to walk.
    struct AdviceMatch {
        CajetaClassPtr aspectClass;     // declaring @Aspect class
        MethodPtr adviceMethod;         // method annotated @Before/etc.
        AdviceKind kind;
        PointcutShape shape;
    };

    class MethodCallParameter;

    struct ParameterEntry {
        CajetaTypePtr type;
        llvm::Value* value;
        string label;
        ParameterEntry(CajetaTypePtr type, string label, llvm::Value* value) { this->type = type; this->label = label; this->value = value; }
        ParameterEntry(const ParameterEntry& src) { type = src.type; label = src.label; value = src.value; }
    };

    class Method : public Modifiable, public Annotatable, public std::enable_shared_from_this<Method> {
    protected:
        static thread_local map<string, MethodPtr> archive;  // per-compile
        string name;
        // Declaration name-token position; see getDeclLine(). 0 = synthesized.
        int declLine = 0;
        int dbgLineDelta = 0;
        int declColumn = 0;
        CajetaClassPtr parent;
        CajetaTypePtr returnType;
        // A `#` return: the method transfers ownership of the result (MemoryModel.md).
        bool returnsOwnership = false;
        // A `^` return — the VIEW stance: the result is interior to the
        // receiver, so the caller must not free it and no return flag is
        // written. Mutually exclusive with returnsOwnership.
        bool returnsView = false;
        // Injected by a member synthesizer, not user-authored.
        bool synthesizedMember = false;
        // Cached value-return body scan: -1 unknown, 0 false, 1 true. A method
        // returns a stack value iff some `return stack X(...)` makes the result
        // an sret + NRVO copy — storage class lives on the construction.
        int returnsStackValueCache = -1;
        uint64_t prototypeEpochSeen = 0;
        // The incoming hidden transfer-word argument (trailing i64), stashed at
        // prologue binding; null before body codegen or without the word.
        llvm::Value* transferWordArg = nullptr;
        BlockPtr block;
        bool constructor;
        // Abstract: no body, no LLVM function — it carries the signature alone.
        bool abstractFlag = false;
        // Varargs (`T... args`): a flag, since the declared `T[]` cannot say so.
        bool varargsFlag = false;
        map<string, FormalParameterPtr> parameters;
        vector<FormalParameterPtr> parameterList;
        // `throws` clause, advisory only — the lint pass warns at call sites.
        vector<QualifiedNamePtr> throwsList;
        int virtualTableIndex;

        // Method-level templates: type parameters alone = a declaration (no LLVM
        // function; methodSource holds its text for re-parse), both non-empty =
        // a concrete instantiation, both empty = an ordinary method.
        vector<TypeParameter> methodTypeParameters;
        vector<CajetaTypePtr> methodTypeArguments;
        string methodSource;
        // Per-method instantiation cache keyed by `<argCanonical,...>`; the
        // template Method alone holds one, since instantiations do not recurse.
        map<string, MethodPtr> methodInstantiationCache;
        // Reuse epoch this cache was valid for. When it lags getReuseEpoch() the
        // entries are bound to a freed user emit module and are cleared on use.
        uint64_t methodInstantiationCacheEpoch = 0;

        // Entry-block slot holding this method's debug-frame NODE, so each
        // frame_leave unlinks exactly this invocation's frame.
        llvm::Value* dbgFrameSlot = nullptr;

        // Did this method's PROLOGUE emit __cajeta_line_enter? A lambda body is
        // emitted inline and never runs that prologue, and the shadow leave takes
        // no argument — an unpaired one pops the ENCLOSING method's frame.
        bool lineFrameEmitted = false;

        // Exact-instrumentation probe pair (descriptor + timestamp slot). Empty
        // when the profiler is off or this class is outside the selection.
        prof::ProfileFrame profFrame;
    public:
        llvm::Value* getDbgFrameSlot() const { return dbgFrameSlot; }
        bool hasLineFrame() const { return lineFrameEmitted; }
        void setHasLineFrame(bool v) { lineFrameEmitted = v; }
        const prof::ProfileFrame& getProfileFrame() const { return profFrame; }
    protected:

        // Stack of drop frames: Block::generateCode pushes one at entry,
        // registers the owned locals declared inside, and fires + pops it at the
        // closing `}` — which is what releases RAII guards at inner-block exit.
        vector<vector<llvm::Value*>> dropFrameStack;

        CajetaModulePtr module;
        // Emit target — the llvm::Module this method's IR is CREATED in. Null
        // means emission coincides with `module` (production); it is set only in
        // the stdlib test-reuse path, where a user-typed instantiation moves.
        CajetaModulePtr emitModule;
        llvm::IRBuilder<>* builder;
        llvm::FunctionType* llvmFunctionType = nullptr;
        llvm::Function* llvmFunction = nullptr;
        // With an @Around advice matched, the user body emits into this
        // separately-named function (canonical + `__original`) and llvmFunction
        // becomes the wrapper that passes it as the `@Original` proceed argument.
        llvm::Function* llvmOriginalFunction = nullptr;
        llvm::BasicBlock* llvmBasicBlock;
        // Implicit function-body scope: entry stores __cajeta_scope_save_top()
        // here, and every return path exits to it before the ret.
        llvm::AllocaInst* scopeWatermark = nullptr;
        // Frame-arena mark taken at method entry; every method-exit edge resets
        // the arena to it, reclaiming arena locals whose own block reset was
        // skipped by a terminator. Null when the method has no arena locals.
        llvm::Value* methodArenaMark = nullptr;
        vector<AdviceMatch> matchingAdvice;
    public:
        Method(CajetaModulePtr module,
            string& name,
            CajetaTypePtr returnType,
            vector<FormalParameterPtr> parameters,
            BlockPtr block,
            CajetaClassPtr parent);

        Method(CajetaModulePtr module,
            string name,
            CajetaTypePtr returnType,
            CajetaClassPtr parent);

        // Frozen-aware binding accessors (frozen = the parent class is), defined
        // in Method.cpp. Reference-returning: call sites read and write through.
        bool isFrozen() const;
        static std::unordered_map<const Method*, MethodLlvmBindings>& frozenMethodBindings();
        llvm::Function*& llvmFunctionRef();
        llvm::FunctionType*& llvmFunctionTypeRef();
        llvm::Function*& llvmOriginalFunctionRef();

        // Recompute the prototype when a placeholder filled since it was built —
        // such a signature changes ABI once the real declarations arrive.
        void ensureFreshPrototype();

        llvm::FunctionType* getLlvmFunctionType() {
            ensureFreshPrototype();
            return llvmFunctionTypeRef();
        }

        llvm::AllocaInst* getScopeWatermark() const { return scopeWatermark; }

        // True iff the body lexically contains a spawn/detach/scope and so needs
        // the per-method scope frame; spawn-free bodies elide its heap churn.
        bool bodyNeedsScopeFrame();

        const vector<QualifiedNamePtr>& getThrowsList() const { return throwsList; }
        void setThrowsList(vector<QualifiedNamePtr> list) { throwsList = std::move(list); }

        void addAdviceMatch(AdviceMatch m) {
            matchingAdvice.push_back(std::move(m));
        }
        const vector<AdviceMatch>& getMatchingAdvice() const {
            return matchingAdvice;
        }
        vector<AdviceMatch>& getMutableMatchingAdvice() {
            return matchingAdvice;
        }

        // Fire the matching @Before advice (right after scope_enter) and @After
        // advice (at every exit, ahead of the scope-exit and drop calls).
        void emitBeforeAdvice(CajetaModulePtr module);
        void emitAfterAdvice(CajetaModulePtr module);

        // Emit entry-point null checks for every @NonNull pointer parameter,
        // skipping the implicit `this` and non-reference slots.
        void emitNonNullParamChecks(CajetaModulePtr module);
        // @AfterReturning fires on the normal-return path only; @AfterThrowing
        // from inside the catch arm of the try/catch wrapping. Both no-arg in v1.
        void emitAfterReturningAdvice(CajetaModulePtr module);
        void emitAfterThrowingAdvice(CajetaModulePtr module);
        bool hasAfterThrowingAdvice() const;

        // Push an exception frame + setjmp and return its blocks; the storage is
        // alloca'd in the entry block, and the builder is left in `tryBB`.
        struct TryFrameInfo {
            llvm::BasicBlock* tryBB;
            llvm::BasicBlock* catchBB;
            llvm::Value* framePtr;
        };
        TryFrameInfo emitAfterThrowingTryEntry(
            CajetaModulePtr module, llvm::IRBuilder<>& wb,
            llvm::Function* parentFn);
        // Pop the try frame at a normal exit; guard with hasAfterThrowingAdvice.
        void emitAfterThrowingTryPop(CajetaModulePtr module);
        // Catch-arm body: take the thrown value, pop the frame, fire
        // @AfterThrowing + @After, re-raise. Set the insert point to catchBB.
        void emitAfterThrowingCatchArm(
            CajetaModulePtr module, llvm::IRBuilder<>& wb);

        // Emit the @Around wrapper once the body is in llvmOriginalFunction:
        // @Before, then the advice proceeding through it, then @After.
        void emitAroundWrapper();

        llvm::Function* getLlvmFunction() { return llvmFunctionRef(); }
        // Reuse-cache only: reset the cached llvm::Function* to its stdlib-prime
        // value so the next test regenerates into ITS module. Not for codegen.
        void setLlvmFunction(llvm::Function* f) { llvmFunctionRef() = f; }
        // Extracted body of an @Around-wrapped method; null otherwise.
        llvm::Function* getLlvmOriginalFunction() { return llvmOriginalFunction; }

        bool isConstructor() { return constructor; }

        bool isAbstract() const { return abstractFlag; }
        void setAbstract(bool v) { abstractFlag = v; }

        bool isVarargs() const { return varargsFlag; }
        void setVarargs(bool v) { varargsFlag = v; }

        // isMethodTemplate(): declares type parameters and is not instantiated
        // (no LLVM function, no vtable slot). The other: a monomorphization.
        bool isMethodTemplate() const {
            return !methodTypeParameters.empty() && methodTypeArguments.empty();
        }
        bool isMethodTemplateInstantiation() const {
            return !methodTypeParameters.empty() && !methodTypeArguments.empty();
        }
        const vector<TypeParameter>& getMethodTypeParameters() const {
            return methodTypeParameters;
        }
        void setMethodTypeParameters(vector<TypeParameter> params) {
            methodTypeParameters = std::move(params);
        }
        const vector<CajetaTypePtr>& getMethodTypeArguments() const {
            return methodTypeArguments;
        }
        void setMethodTypeArguments(vector<CajetaTypePtr> args) {
            methodTypeArguments = std::move(args);
        }
        const string& getMethodSource() const { return methodSource; }
        void setMethodSource(string src) { methodSource = std::move(src); }

        // Per-call instantiation: re-parse the captured source under
        // (methodTypeParameters[i] -> args[i]), emit a mangled LLVM function and
        // cache by arg-list canonical. Template-only; records cross-module use.
        MethodPtr instantiateMethodTemplate(vector<CajetaTypePtr> args);

        // A function-typed parameter dropped from a specialized instance and
        // bound to a known function, so a call to `name` lowers DIRECT.
        struct BoundClosureBinding {
            string name;
            CajetaTypePtr fnType;
            llvm::Function* fn;
            llvm::Constant* record;   // the { fn, null, null } closure global
        };
        struct ClosureSpecialization {
            string paramName;
            llvm::Function* fn;
            CajetaTypePtr fnType;
            llvm::Constant* record;
        };

        // Build (or cache-hit) the specialized instance F<args>$fn: instantiate,
        // then drop `paramName` and bind it. NOT in the class method maps.
        MethodPtr instantiateSpecializedClosure(vector<CajetaTypePtr> args,
            const string& paramName, llvm::Function* fn, CajetaTypePtr fnType,
            llvm::Constant* record);

        const vector<BoundClosureBinding>& getBoundClosures() const { return boundClosures; }
        const string& getSpecializationTag() const { return specializationTag; }
        // The template this instance came from; null on templates and ordinary.
        Method* getTemplateOrigin() const { return templateOrigin; }
        void setTemplateOrigin(Method* t) { templateOrigin = t; }
        void dropParameter(const string& name);
        void addBoundClosure(const string& name, CajetaTypePtr fnType,
                             llvm::Function* fn, llvm::Constant* record) {
            boundClosures.push_back({name, std::move(fnType), fn, record});
        }
        void setSpecializationTag(const string& tag) { specializationTag = tag; }

    private:
        MethodPtr instantiateMethodTemplateInternal(vector<CajetaTypePtr> args,
            const ClosureSpecialization* spec = nullptr);
        vector<BoundClosureBinding> boundClosures;
        string specializationTag;
        Method* templateOrigin = nullptr;

    public:

        // Reparent an instantiation off the throwaway class that hosted its re-parse.
        void setParentForInstantiation(CajetaClassPtr p) { parent = p; }

        // Point an instantiation's method at a different emit module (stdlib
        // test-reuse). Must be called BEFORE generatePrototype emits anything.
        void setModuleForInstantiation(CajetaModulePtr m) { module = m; }

        vector<FormalParameterPtr> getParameterList() { return parameterList; }

        map<string, FormalParameterPtr> getParameters() { return parameters; }

        /** Prepend an explicit `this` of type `t` unless slot 0 already holds
         *  one. Enum bodies need it: the receiver is an i32 ordinal, and
         *  generatePrototype would otherwise splice in a pointer `this`. */
        void prependThisParameter(const CajetaTypePtr& t) {
            if (!parameterList.empty() && parameterList.front()
                    && parameterList.front()->getName() == "this") {
                return;
            }
            auto p = make_shared<FormalParameter>(string("this"), t);
            p->setParent(shared_from_this());
            parameterList.insert(parameterList.begin(), p);
            parameters[p->getName()] = p;
        }

        CajetaTypePtr getReturnType() { return returnType; }

        CajetaClassPtr getParent() const { return parent; }

        bool isReturnsOwnership() const { return returnsOwnership; }
        void setReturnsOwnership(bool v) { returnsOwnership = v; }
        bool isReturnsView() const { return returnsView; }
        void setReturnsView(bool v) { returnsView = v; }

        // needsTransferWord(): the LLVM signature carries a hidden TRAILING i64
        // whose bit i says user-arg i surrendered a title (true iff any formal
        // passes by pointer). returnsClassPointer(): a plain `ret ptr` return.
        bool needsTransferWord();
        bool returnsClassPointer();
        // May a caller trust the return-flag TLS right after this call? Raw-IR
        // synthesized bodies never store the flag and override this to false.
        virtual bool emitsReturnFlag() { return returnsClassPointer(); }
        llvm::Value* getTransferWordArg() const { return transferWordArg; }
        void setTransferWordArg(llvm::Value* v) { transferWordArg = v; }
        // Seed a drop entry per droppable class-typed formal, armed from its
        // transfer-word bit. Prologue-only; a no-op without the word.
        void emitFormalDropEntries(CajetaModulePtr module);

        // Does the body RETAIN this formal — store it into a field or element?
        // Only a retaining callee can hold a lend of the caller's local. Cached.
        bool retainsFormal(const std::string& formalName);

        // Last-use advisory: true when `name` has no later read than (line,
        // column). Uses inside a loop never qualify — a textual last use runs
        // again next iteration. Advisory only: a warning with a `#` fixit.
        bool isFinalUseOfLocal(const std::string& name, int line, int column);
    private:
        map<std::string, bool> retainsFormalCache;
        // name -> last (line, column) read; loop-used names are never advised.
        map<std::string, pair<int, int>> lastUseAt;
        set<std::string> loopUsedNames;
        bool lastUsesComputed = false;
        void computeLastUses();
    public:

        bool isSynthesizedMember() const { return synthesizedMember; }
        void setSynthesizedMember(bool v) { synthesizedMember = v; }

        // True iff this method returns a `stack`-constructed value by copy (the
        // sret + NRVO ABI). Lazily scanned from the body, then cached.
        bool returnsStackValue();

        // Shared body-scan helpers, exposed for lambdas, which ask the same
        // return-by-stack-value question to pick their ABI. In Method.cpp.
        static bool exprIsStackConstruction(const ExpressionPtr& e);
        static bool nodeHasStackReturn(const AbstractSyntaxNodePtr& node);
        static bool blockHasStackReturn(const BlockPtr& block);

        // The value-shape-by-convention return class, `cajeta.lang.Optional<T>`:
        // a method or lambda returning it is ALWAYS sret whatever its body does,
        // because the body scan cannot see a relay (`return other.find();`).
        static bool isValueShapeReturnType(const CajetaTypePtr& t);

        // True iff the body PROVES its plain (non-`#`) result is a window into
        // the receiver's interior: every return is a `this.field` read or index.
        bool returnsInteriorView() const;
        /// True when every return is a kind-decidable title (a fresh value or a
        /// concatenation), so the caller's flag read folds away.
        bool returnsStaticTitle() const;
        static bool exprIsStaticTitle(const ExpressionPtr& e);
        static bool nodeReturnsOnlyStaticTitles(const AbstractSyntaxNodePtr& node,
                                                bool& sawReturn);
        static bool exprIsInteriorRead(const ExpressionPtr& e);
        static bool nodeReturnsOnlyInteriorViews(
            const AbstractSyntaxNodePtr& node, bool& sawView);

        // Lint: a `#Optional<T>` method returning only `heap Optional<...>`
        // allocates scope-bounded memory a `stack` value return would avoid.
        void lintHeapOptionalReturn();
        // Lint: a plain-`T` method whose every return provably yields a title
        // should declare `#T` (IDE copy of FRESH_RETURN_NEEDS_TRANSFER).
        void lintPlainReturnYieldsTitle();
        // Lint: a plain return of `a[i]` from a frame-owned array is a borrow the
        // frame's own drop frees before the caller reads it. Never an error.
        void lintPlainReturnOfOwnedSlot();

        // ---- Frame-arena eligibility ---------------------------------------
        // Owned String-concat locals proven non-escaping: they bump-allocate
        // from the frame arena and register no drop entry (usesArena() gates it).
        std::set<std::string> arenaEligibleNames;
        bool bodyResolved = false;
        bool methodUsesArena = false;
        bool usesArena() const { return methodUsesArena; }
        bool isArenaEligibleLocal(const std::string& n) const {
            return arenaEligibleNames.find(n) != arenaEligibleNames.end();
        }
        // Walk the body: flag non-escaping String-concat nodes arena-eligible.
        void computeArenaEligibility();

        // The body's type-resolver pass, callable on its own so `--lint` (which
        // stops before codegen) still records call edges and field references.
        // IDEMPOTENT: a second call is a no-op, not a second walk.
        void resolveBody(CajetaModulePtr module);
        bool isBodyResolved() const { return bodyResolved; }

        // Resolve this body from OUTSIDE codegen — what `--lint` calls. Body
        // resolution assumes the context codegen's prologue establishes, so this
        // pushes the owning class and a scope of the formals, then tears it down.
        void resolveBodyForLint(CajetaModulePtr module);
        static int64_t bodyResolveWalks();

        // Push a fresh drop frame; Block::generateCode calls this at its entry.
        void pushDropFrame() { dropFrameStack.emplace_back(); }
        size_t dropFrameCount() const { return dropFrameStack.size(); }
        // break/continue: emit pop_run for every frame deeper than `keep`
        // (inner -> outer), leaving the compile-time stack itself untouched.
        void emitFrameDropsToDepth(CajetaModulePtr module, size_t keep);

        // Pop the top frame without emitting IR, after its drops or a terminator.
        void popDropFrame() {
            if (!dropFrameStack.empty()) dropFrameStack.pop_back();
        }

        // Register an owned local's DropEntry alloca into the current top frame.
        void registerDropEntry(llvm::Value* entry) {
            if (dropFrameStack.empty()) {
                dropFrameStack.emplace_back();
            }
            dropFrameStack.back().push_back(entry);
        }

        // Emit the top frame's drops in reverse order, at a block's normal
        // closing `}`. The frame is NOT popped — call popDropFrame() afterwards.
        void emitTopFrameDrops(CajetaModulePtr module);

        // Emit drops for EVERY frame still on the stack, innermost to outermost.
        // Called by ReturnStatement before the ret; the frames themselves stay.
        void emitOwnerDrops(CajetaModulePtr module);

        // Maps a snippet line to the true file line for a body re-parsed from a
        // template snippet; 0 on ordinary (non-instantiated) methods.
        int getDbgLineDelta() const { return dbgLineDelta; }
        void setDbgLineDelta(int d) { dbgLineDelta = d; }

        int getDeclLine() const { return declLine; }
        int getDeclColumn() const { return declColumn; }

        void setDeclPosition(int line, int column) {
            declLine = line;
            declColumn = column;
        }

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        int getVirtualTableIndex() {
            return virtualTableIndex;
        }

        void setVirtualTableIndex(int virtualTableIndex) {
            this->virtualTableIndex = virtualTableIndex;
        }

        bool operator<(const Method& src) const {
            return virtualTableIndex < src.virtualTableIndex;
        }

        void destroyScope();

        FieldPtr getVariable(string name);

        CajetaModulePtr getModule() { return module; }

        // The module this method's IR is CREATED in: emitModule, else the
        // declaring class's emit module, else the resolution module.
        CajetaModulePtr getEmitModule();
        void setEmitModule(CajetaModulePtr m) { emitModule = m; }

        const string toCanonical(bool labeled = false) {
            return buildCanonical(parent, name, parameterList, labeled);
        }

        const string toGeneric(bool labeled = false) {
            return buildGeneric(parent, name, parameterList, labeled);
        }

        // Map-storage key: `toCanonical(labeled)`, except a method-template
        // INSTANTIATION appends `<argCanonical,...>` to stay distinct.
        const string getMapKey(bool labeled = false) const;

        // LLVM symbol name, mirroring getMapKey(true) so instantiations get
        // distinct functions; only they carry the type-arg suffix.
        const string getLlvmSymbolName() const;

        void generatePrototype();

        void setBlock(BlockPtr block);

        BlockPtr getBlock() const { return block; }

        void createScope();

        void createLocalVariable(CajetaModulePtr module, FieldPtr field);

        void setLocalVariable(CajetaModulePtr module, string name, llvm::Value* value);

        virtual void generateCode();

        // Emit a thin forwarding wrapper to the C-runtime symbol `symbol`, for
        // a method carrying @Native.
        void emitNativeForwardingBody(const std::string& symbol);
        // Record a native-library requirement (lib-id, symbol) as named metadata
        // `cajeta.native.reqs` on the emit module, deduped.
        void recordNativeRequirement(const std::string& lib,
                                     const std::string& symbol);

        static map<string, MethodPtr>& getArchive();

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled);

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled);

        // Same shape as buildCanonical, but un-substitutes the typeArguments
        // back to the template's parameter names: a substitution-stable hash.
        static string buildTemplateOriginCanonical(
            CajetaClassPtr instClass,
            const string& name,
            vector<FormalParameterPtr> parameters,
            bool labeled);

        static string buildGeneric(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled);

        static string buildGeneric(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled);

        static MethodPtr create(CajetaModulePtr module,
            string& name,
            CajetaTypePtr returnType,
            vector<FormalParameterPtr> parameters,
            BlockPtr block,
            CajetaClassPtr parent);

        static MethodPtr create(CajetaModulePtr module,
            string name,
            CajetaTypePtr returnType,
            CajetaClassPtr parent);
    };
}


