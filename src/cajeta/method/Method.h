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
#include "queue"
#include "map"
#include "unordered_map"

using namespace std;

namespace cajeta {
    class CajetaModule;

    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Expression;

    class CajetaClass;

    // U6.3b — a method's LLVMContext-bound codegen bindings. When the method's
    // parent class is frozen (shared, immutable), each compile thread keeps its
    // own copy in a per-thread side-table (frozenMethodBindings()) because these
    // pointers belong to a per-thread LLVMContext. Behaviour is unchanged while
    // not frozen — the inline members are used. Mirrors ClassLlvmBindings.
    struct MethodLlvmBindings {
        llvm::FunctionType* llvmFunctionType = nullptr;
        llvm::Function* llvmFunction = nullptr;
        llvm::Function* llvmOriginalFunction = nullptr;
    };

    typedef shared_ptr<CajetaClass> CajetaClassPtr;

    class Mathod;

    typedef shared_ptr<Method> MethodPtr;

    // Advice kinds — which `@Before` / `@After` / etc. annotation
    // sits on the advice method. A3's pointcut-matching pass tags
    // each AdviceMatch with one of these so A4+'s codegen can pick
    // the right wrapper shape per advice form.
    enum class AdviceKind {
        Before,
        After,
        Around,
        AfterReturning,
        AfterThrowing,
    };

    // Pointcut shape — how the advice method's pointcut argument
    // resolved. Marker-annotation pointcut: the advice fires on
    // every method annotated with the named annotation type.
    // Type-based pointcut: the advice fires on every method on the
    // named class or any subclass. v1's two recognized shapes.
    enum class PointcutShape {
        MarkerAnnotation,
        Type,
    };

    // One (aspect, advice method, advice kind, pointcut shape)
    // tuple cached on a user Method that the pointcut-matching
    // pass deemed a match. A4+ walks the list at codegen time to
    // generate the appropriate wrapper.
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
        static thread_local map<string, MethodPtr> archive;  // per-compile (U3)
        string name;
        // Declaration name-token position; see getDeclLine(). 0 = synthesized.
        int declLine = 0;
        // See getDbgLineDelta() — snippet-line -> file-line correction for
        // template-instantiated bodies (9.2).
        int dbgLineDelta = 0;
        int declColumn = 0;
        CajetaClassPtr parent;
        CajetaTypePtr returnType;
        // True iff the return type is prefixed with `#`, meaning the method
        // transfers ownership of the returned value to its caller. See
        // `MemoryModel.md` § Function signatures.
        bool returnsOwnership = false;
        // True iff the return type is prefixed with `^` — the VIEW stance
        // (spec §4.7): the result is interior to the receiver, the caller must
        // not free it, and the method emits no return-flag write because the
        // flag is statically 0. Mutually exclusive with `returnsOwnership`;
        // the grammar makes them alternatives of one optional prefix.
        //
        // This is the only one of the three stances that is decidable from the
        // SIGNATURE. `#` and plain `T` both need the return statement's
        // provenance to say what actually came back, which is why §4.5's guard
        // reaches only the shapes whose provenance was recorded (plan 8.2.13's
        // probe: a call result, a formal and an interior read all slip past).
        bool returnsView = false;
        // True when this method was injected by a registered member
        // synthesizer (source-synthesis facility). Consulted by checks that
        // distinguish compiler-generated members from user-authored ones —
        // e.g. the record add-but-not-redefine shadow ban exempts a shadow of
        // a SYNTHESIZED parent method (each record level gets its own typed
        // `clone()`; static dispatch picks by declared type, so the shadow is
        // benign).
        bool synthesizedMember = false;
        // Cached result of the value-return body scan. -1 = not computed,
        // 0 = false, 1 = true. A method "returns a stack value" iff (it does
        // not transfer ownership and) a return statement hands back a `stack`
        // construction (`return stack X(...)`), which makes the return a
        // by-value copy lowered through the sret + NRVO ABI rather than a
        // pointer. Storage class lives on the construction expression, so this
        // scan is the single source of truth — there is no `stack T` return
        // type. See docs/specification/lang/ValueReturns.md.
        int returnsStackValueCache = -1;
        // CajetaClass::typeFillEpoch() value this method's prototype was
        // computed at; ensureFreshPrototype re-derives when stale.
        uint64_t prototypeEpochSeen = 0;
        // title-tracking Unit 5: the incoming hidden transfer-word argument
        // (trailing i64), stashed at prologue binding so callee-side codegen
        // (runtime-owner formals, 5.2.2) can read per-formal bits. Null when
        // needsTransferWord() is false or before body codegen.
        llvm::Value* transferWordArg = nullptr;
        BlockPtr block;
        bool constructor;
        // Abstract method — has no body, no LLVM function declaration.
        // Interface methods are abstract; the implementing class's matching
        // method is what actually executes at dispatch time. The abstract
        // method's only role is to carry the signature (for canonical
        // computation + vtable-hash lookup).
        bool abstractFlag = false;
        // Varargs (`T... args`) method. The last parameter's type is `T[]`;
        // call sites with more than (parameterList.size() - 1) args pack
        // the trailing args into a fresh array before passing. Tracked as a
        // flag because the parameter type alone (`T[]`) can't distinguish
        // a varargs slot from a regular array-typed slot.
        bool varargsFlag = false;
        map<string, FormalParameterPtr> parameters;
        vector<FormalParameterPtr> parameterList;
        // RecoverableException subtypes the method documents itself as
        // throwing (Java-style `throws T1, T2` clause). Advisory only —
        // no compile-time enforcement; the lint pass uses this list to
        // emit uncaught-throws warnings at call sites. UnrecoverableException
        // subtypes are not declared here (per ErrorModel.md). The list
        // stores QualifiedNamePtrs because the named types may not have
        // been resolved at parse time; resolution happens lazily when the
        // lint pass walks them.
        vector<QualifiedNamePtr> throwsList;
        int virtualTableIndex;

        // Method-level templates (docs/specification/lang/templates/MethodLevelTemplate.md).
        // `methodTypeParameters` non-empty AND `methodTypeArguments` empty =
        // a method-template declaration (no LLVM function emitted; body source
        // captured for re-parse at call sites that instantiate it). Both non-
        // empty = a concrete instantiation produced by per-call monomorphization.
        // Both empty = an ordinary non-templated method.
        //
        // `methodSource` captures the raw text of just the methodDeclaration
        // rule (from `final <R>` through the closing brace) so the body can
        // be re-parsed with substitutions pushed. Per-call cache keys the
        // resulting Method on the parent class.
        vector<TypeParameter> methodTypeParameters;
        vector<CajetaTypePtr> methodTypeArguments;
        string methodSource;
        // Per-method instantiation cache. Keyed by `<argCanonical,...>` over
        // methodTypeArguments. Only populated on the template Method (the
        // one with empty methodTypeArguments); instantiations themselves
        // don't recurse.
        map<string, MethodPtr> methodInstantiationCache;
        // Reuse epoch this cache was last valid for. When it lags
        // CajetaModule::getReuseEpoch() (a new test in the reuse path), the
        // cache holds instantiations bound to a freed user emit module and is
        // cleared on next use. Unused (epoch never bumps) in production.
        uint64_t methodInstantiationCacheEpoch = 0;

        // resident-debug-server 9.1: entry-block slot holding this
        // method's debug-frame NODE (the prolog's frame_enter result);
        // every frame_leave (returns + fall-through) loads it so leave
        // unlinks exactly this invocation's frame. Reset per generateCode.
        llvm::Value* dbgFrameSlot = nullptr;
    public:
        llvm::Value* getDbgFrameSlot() const { return dbgFrameSlot; }
    protected:

        // Stack of drop frames. Each Block::generateCode pushes a frame
        // at entry, registers any owned locals declared inside into the
        // top frame, and fires + pops the frame's entries at the
        // block's closing `}` (unless the block was already terminated
        // by a return/throw — those fire ALL frames LIFO themselves
        // before the terminator). This is what makes RAII guards
        // release at inner-block exit instead of at method exit. See
        // MemoryModel.md § Runtime: drop chain with watermark.
        vector<vector<llvm::Value*>> dropFrameStack;

        CajetaModulePtr module;
        // Emit target — the llvm::Module that this method's Function/IR is
        // CREATED in. Defaults to null (→ getEmitModule() returns `module`, so
        // resolution and emission coincide, the production behavior). Set only
        // in the stdlib test-reuse path: a stdlib-template instantiation over a
        // USER type keeps `module` = the stdlib template module (name resolution
        // needs its imports/substitution) but sets emitModule = the user module,
        // so its IR lands there and the cached stdlib module stays pristine.
        // Context is shared in reuse mode, so only Function/global CREATION sites
        // (module->getLlvmModule()) consult getEmitModule(); getLlvmContext()
        // (type creation) and getBuilder() (context-bound insert point) do not.
        CajetaModulePtr emitModule;
        llvm::IRBuilder<>* builder;
        // Initialized to null so getLlvmFunction()/getLlvmFunctionType() return
        // null (not uninitialized garbage) before generatePrototype runs — a
        // method may be reached (e.g. reflection's adapter pass over every
        // class) before it has been prototyped.
        llvm::FunctionType* llvmFunctionType = nullptr;
        llvm::Function* llvmFunction = nullptr;
        // A5 method extraction: when at least one @Around advice
        // matches this method, the user-written body emits into THIS
        // separately-named llvm Function (canonical + `__original`
        // suffix), and llvmFunction becomes the wrapper whose body
        // calls the advice with this function pointer as the
        // `@Original` proceed argument. Null on methods that don't
        // need wrapping — the body emits directly into llvmFunction
        // and external callers' getLlvmFunction() resolves to the
        // same direct entry point.
        llvm::Function* llvmOriginalFunction = nullptr;
        llvm::BasicBlock* llvmBasicBlock;
        // R5-A' implicit function-body scope: at function entry codegen
        // alloca's a ptr slot here and stores `__cajeta_scope_save_top()`
        // into it. Each return path loads it back and calls
        // `__cajeta_scope_exit_to(watermark)` so every scope frame
        // pushed inside the function — implicit function-body OR any
        // explicit `scope { }` the return is inside of — gets waited
        // and popped before the ret instruction.
        llvm::AllocaInst* scopeWatermark = nullptr;
        // Frame-arena: the arena bump position captured at method entry. On every
        // method-exit edge (emitOwnerDrops — explicit + fall-through return) the
        // arena is reset to this mark, reclaiming any arena-routed locals whose own
        // block reset was skipped because the block ended in a terminator (a
        // return). Without it those reclaims (and their dropCount ticks) deferred
        // to an enclosing scope — which for a top-level call is the C++ caller, so
        // they never fired. Null when the method has no arena locals.
        llvm::Value* methodArenaMark = nullptr;
        // Advice matches: each entry says "this aspect's advice
        // method applies to me." Populated by the pointcut-matching
        // pass (AspectModel.md § A3) that runs once per Compiler
        // after parse, before codegen. Read by A4+'s codegen wrapper.
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

        // U6.3b — frozen-aware binding accessors. A method is "frozen" when its
        // parent class is frozen; the check + the side-table live in Method.cpp
        // (CajetaClass is incomplete here). While not frozen they alias the inline
        // members (behaviour-identical to pre-U6.3b). Reference-returning so call
        // sites read and write through them. Defined in Method.cpp.
        bool isFrozen() const;
        static std::unordered_map<const Method*, MethodLlvmBindings>& frozenMethodBindings();
        llvm::Function*& llvmFunctionRef();
        llvm::FunctionType*& llvmFunctionTypeRef();
        llvm::Function*& llvmOriginalFunctionRef();

        // Recompute the prototype when a placeholder filled since it was
        // built (specs/record-cross-type-return): the signature of a method
        // whose param/return types were placeholders can change ABI once the
        // real declarations arrive. Cheap when fresh (epoch compare).
        // Defined in Method.cpp.
        void ensureFreshPrototype();

        llvm::FunctionType* getLlvmFunctionType() {
            ensureFreshPrototype();
            return llvmFunctionTypeRef();
        }

        llvm::AllocaInst* getScopeWatermark() const { return scopeWatermark; }

        // SIMD plan Phase 0.6: true iff the body lexically contains a
        // spawn/detach/scope and therefore needs the per-method scope frame.
        // Spawn-free bodies elide it (the frame heap-allocs + churns per call).
        bool bodyNeedsScopeFrame();

        const vector<QualifiedNamePtr>& getThrowsList() const { return throwsList; }
        void setThrowsList(vector<QualifiedNamePtr> list) { throwsList = std::move(list); }

        // Advice-match cache (AspectModel.md § A3). The pointcut-
        // matching pass appends; A4+ codegen iterates. Empty for
        // every method by default — only the matching pass populates.
        void addAdviceMatch(AdviceMatch m) {
            matchingAdvice.push_back(std::move(m));
        }
        const vector<AdviceMatch>& getMatchingAdvice() const {
            return matchingAdvice;
        }
        // Mutable accessor for the A7 sort pass — A3 appends in
        // pointcut-match order, then resolveAdviceMatches restabilizes
        // by @Order. Consumers in A4/A6 still iterate via the const
        // getter and see the final order.
        vector<AdviceMatch>& getMutableMatchingAdvice() {
            return matchingAdvice;
        }

        // A4 codegen wrappers. emitBeforeAdvice fires every matching
        // @Before advice at the current insert point (called from
        // Method::generateCode right after scope_enter, before the
        // body emits). emitAfterAdvice fires every matching @After
        // advice (called from Method::generateCode at fall-through
        // exit and from ReturnStatement::generateCode at every
        // explicit return — both BEFORE the existing
        // emitScopeExitToWatermark / emitOwnerDrops calls, so the
        // advice runs in the method-body lifetime).
        //
        // v1 advice constraints (relaxed in A5+): static method,
        // no parameters, void return. Methods that don't match are
        // silently skipped — A12's diagnostics pass surfaces them.
        void emitBeforeAdvice(CajetaModulePtr module);
        void emitAfterAdvice(CajetaModulePtr module);

        // @NonNull (docs/specification/reflect/Annotations.md § Null safety).
        // Emit entry-point null-checks for every @NonNull-annotated
        // pointer parameter. Skipped for the implicit `this` and for
        // non-reference parameters. Called from generateCode after
        // scope setup, before any user-body emission. Lowers to
        // `if (arg == null) throw 2;` (CAJETA_ERROR_NULL_PARAM_ARG).
        void emitNonNullParamChecks(CajetaModulePtr module);
        // A6: @AfterReturning fires only on the normal-return path
        // (same hook points as @After). @AfterThrowing fires only
        // from inside the catch arm of the try/catch wrapping (see
        // hasAfterThrowingAdvice). v1 advice keeps the no-arg shape
        // — the return-value / Throwable parameters from the spec
        // are deferred to a follow-up.
        void emitAfterReturningAdvice(CajetaModulePtr module);
        void emitAfterThrowingAdvice(CajetaModulePtr module);
        bool hasAfterThrowingAdvice() const;

        // A6 try-frame setup. Push an exception frame + setjmp at
        // the current insert point; return the catch basic block
        // (still empty — the caller emits the catch arm separately
        // after the body finishes). The frame's storage is alloca'd
        // in the function entry block to keep setjmp/alloca
        // interaction sane. Reused by Method::generateCode (for
        // non-@Around methods) and emitAroundWrapper (for the
        // wrapper path). The body emits into the "try" block this
        // function leaves the builder pointing at.
        struct TryFrameInfo {
            llvm::BasicBlock* tryBB;
            llvm::BasicBlock* catchBB;
            llvm::Value* framePtr;
        };
        TryFrameInfo emitAfterThrowingTryEntry(
            CajetaModulePtr module, llvm::IRBuilder<>& wb,
            llvm::Function* parentFn);
        // Pops the try frame at a normal-exit point. Called from
        // every normal-return site (Method::generateCode fall-
        // through, ReturnStatement explicit returns) when the
        // enclosing method has @AfterThrowing matched. No-op
        // otherwise — call sites guard with hasAfterThrowingAdvice.
        void emitAfterThrowingTryPop(CajetaModulePtr module);
        // Catch arm body: get the thrown value, pop the frame,
        // fire @AfterThrowing + @After advice, re-raise. The
        // builder is left at an unreachable terminator. Caller
        // should set the insert point to the catchBB before
        // calling this.
        void emitAfterThrowingCatchArm(
            CajetaModulePtr module, llvm::IRBuilder<>& wb);

        // A5: emit the @Around wrapper. Called from generateCode
        // after the original body has been emitted into
        // llvmOriginalFunction. The wrapper fires any @Before
        // advice, calls the @Around advice (passing
        // llvmOriginalFunction as the proceed argument), fires any
        // @After advice, and returns the advice's result. Only one
        // @Around match is honored in v1; @Order-driven chaining
        // of multiple Arounds joins A7.
        void emitAroundWrapper();

        llvm::Function* getLlvmFunction() { return llvmFunctionRef(); }
        // Reuse-cache only (StdlibReuseCache::restoreBaseline): reset the cached
        // module-bound llvm::Function* to its stdlib-prime value (or null) so the
        // next reusing test regenerates the body into ITS module instead of
        // referencing a freed/foreign one. Not for production codegen.
        void setLlvmFunction(llvm::Function* f) { llvmFunctionRef() = f; }
        // Extracted-body function for @Around-wrapped methods. Null
        // unless A5 method extraction kicked in. External callers
        // shouldn't usually need this — the public entry is
        // getLlvmFunction (the wrapper); the original is wired
        // internally by Method::generateCode.
        llvm::Function* getLlvmOriginalFunction() { return llvmOriginalFunction; }

        bool isConstructor() { return constructor; }

        bool isAbstract() const { return abstractFlag; }
        void setAbstract(bool v) { abstractFlag = v; }

        bool isVarargs() const { return varargsFlag; }
        void setVarargs(bool v) { varargsFlag = v; }

        // Method-level template predicates and accessors. `isMethodTemplate()`
        // means the declaration introduces method-level type parameters and
        // hasn't been instantiated yet — no LLVM function is emitted and the
        // method is excluded from vtable build. `isMethodTemplateInstantiation()`
        // means this is a concrete monomorphization produced by an
        // instantiation site.
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

        // Per-call instantiation. Re-parses the captured method source with
        // a substitution map built from (methodTypeParameters[i].name -> args[i])
        // pushed on the module, materializes a concrete MethodPtr, emits its
        // LLVM function with a mangled name, and caches by arg-list canonical.
        // Returns the cached instance on repeat calls.
        //
        // Only valid on a template method (asserts isMethodTemplate()).
        // Implemented in MethodTemplateInstantiator.cpp.
        //
        // Thin choke point: forwards to instantiateMethodTemplateInternal and
        // then records a cross-module instantiation obligation (incremental
        // compilation — docs/IncrementalCompilation.md). Capture lives
        // in the wrapper so it fires on cache hits too, mirroring the
        // CajetaClass::instantiate / instantiateInternal split.
        MethodPtr instantiateMethodTemplate(vector<CajetaTypePtr> args);

        // cajeta-ir Unit 4 (closure specialization, Approach A). A function-typed
        // parameter dropped from a specialized instance's signature and bound, in
        // the body scope, to a statically-known function (a non-capturing lambda).
        // Codegen pushes a BoundClosureField for each so a call to `name` lowers
        // to a DIRECT call. Empty on every ordinary method.
        struct BoundClosureBinding {
            string name;
            CajetaTypePtr fnType;
            llvm::Function* fn;
            llvm::Constant* record;   // the { fn, null, null } closure global
        };
        // Request passed to the instantiator to build the above.
        struct ClosureSpecialization {
            string paramName;
            llvm::Function* fn;
            CajetaTypePtr fnType;
            llvm::Constant* record;
        };

        // Build (or cache-hit) the specialized instance F<args>$fn: re-parses
        // like a normal instantiation, then drops `paramName` and binds it to
        // `fn` (with `record` = its closure global for forwarded reads). Generic-
        // only (needs methodSource). NOT registered in the class method maps —
        // the call-site redirect (Unit 4c) drives its codegen and calls it.
        MethodPtr instantiateSpecializedClosure(vector<CajetaTypePtr> args,
            const string& paramName, llvm::Function* fn, CajetaTypePtr fnType,
            llvm::Constant* record);

        const vector<BoundClosureBinding>& getBoundClosures() const { return boundClosures; }
        const string& getSpecializationTag() const { return specializationTag; }
        // The template Method this instance was instantiated from (Unit 4c: the
        // call-site redirect needs it to request a closure specialization, since
        // instantiateSpecializedClosure lives on the template). Null on templates
        // and ordinary methods.
        Method* getTemplateOrigin() const { return templateOrigin; }
        void setTemplateOrigin(Method* t) { templateOrigin = t; }
        // Specialization-build mutators (used by the instantiator post-parse).
        void dropParameter(const string& name);
        void addBoundClosure(const string& name, CajetaTypePtr fnType,
                             llvm::Function* fn, llvm::Constant* record) {
            boundClosures.push_back({name, std::move(fnType), fn, record});
        }
        void setSpecializationTag(const string& tag) { specializationTag = tag; }

    private:
        MethodPtr instantiateMethodTemplateInternal(vector<CajetaTypePtr> args,
            const ClosureSpecialization* spec = nullptr);
        // Closure-specialization state (empty/blank on every ordinary method).
        vector<BoundClosureBinding> boundClosures;
        string specializationTag;
        Method* templateOrigin = nullptr;

    public:

        // Used by MethodTemplateInstantiator to reparent an instantiation
        // from the throwaway wrapper class (used to host the re-parse) to
        // the original template's parent. Call sites otherwise shouldn't
        // change a method's parent after construction.
        void setParentForInstantiation(CajetaClassPtr p) { parent = p; }

        // Reparent the emit-target module of an instantiation's method. Used by
        // the stdlib test-reuse path (Design B): a user-triggered stdlib-template
        // instantiation's bodies must emit into the USER module, not the stdlib
        // module, so the cached stdlib module is never mutated. Must be called
        // BEFORE generatePrototype (nothing emitted yet); `module->getLlvmModule()`
        // is the codegen target (Method.cpp ~994/998/1024).
        void setModuleForInstantiation(CajetaModulePtr m) { module = m; }

        vector<FormalParameterPtr> getParameterList() { return parameterList; }

        map<string, FormalParameterPtr> getParameters() { return parameters; }

        /**
         * Prepend an explicit `this` parameter of type `t`, unless one is
         * already at slot 0.
         *
         * For enum bodies: an enum value is an i32 ordinal, not an object, so
         * the receiver must be typed as the enum itself. generatePrototype's
         * own injection would otherwise splice in a `pointer` `this` — it
         * skips precisely when `this` already occupies slot 0, which is what
         * this call arranges — and a pointer receiver is meaningless for an
         * ordinal.
         */
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

        // title-tracking Unit 5 (spec §4.4) — the per-call transfer ABI.
        // needsTransferWord(): this method's LLVM signature carries a hidden
        // TRAILING i64 whose bit i mirrors the moveMask contract (user-arg i
        // surrendered a title). True iff any user formal passes by pointer;
        // @Kernel/@Device methods and static `main` keep the plain C ABI.
        // returnsClassPointer(): the method returns a raw class pointer
        // (`ret ptr`, not sret/interface/value-type) and so participates in
        // the paired return-flag protocol (__cajeta_return_flag_set).
        bool needsTransferWord();
        bool returnsClassPointer();
        // Callers may trust the return-flag TLS right after calling this
        // method: its body runs through the statement pipeline, which pairs
        // every class-pointer return with __cajeta_return_flag_set. Raw-IR
        // synthesized bodies (generateCode overrides) never store the flag
        // and override this to false — callers fall back to static mode.
        virtual bool emitsReturnFlag() { return returnsClassPointer(); }
        llvm::Value* getTransferWordArg() const { return transferWordArg; }
        void setTransferWordArg(llvm::Value* v) { transferWordArg = v; }
        // 5.2.2 — seed a drop entry per droppable class-typed formal, armed
        // from its transfer-word bit; prologue-only, no-op without the word.
        void emitFormalDropEntries(CajetaModulePtr module);

        // 5.2.7 — does this method RETAIN the named formal, i.e. store it into
        // a field or an element? Only a retaining callee can leave its receiver
        // holding a lend of the caller's local (the dangling-lend hazard); a
        // read-only callee (`sb.append(s)`, `list.contains(x)`) cannot, and
        // must not poison its receiver. Cached; false for bodyless methods.
        bool retainsFormal(const std::string& formalName);

        // 5.2.8 (spec §7.1) — last-use advisory. True when the identifier `name`
        // has no later read in this body than (line, column), i.e. the site is
        // its FINAL use and lending there is probably meant as a transfer. Uses
        // inside a loop body never qualify: a textual last use still runs again
        // on the next iteration, so advising `#` there would be wrong. Advisory
        // only — the compiler cannot know intent (spec §4.6.4 rejects inferring
        // it), so this reports a WARNING with a `#` fixit and never an error.
        bool isFinalUseOfLocal(const std::string& name, int line, int column);
    private:
        map<std::string, bool> retainsFormalCache;
        // name -> last (line, column) it is read at; names used inside any loop
        // are parked in loopUsedNames and never advised.
        map<std::string, pair<int, int>> lastUseAt;
        set<std::string> loopUsedNames;
        bool lastUsesComputed = false;
        void computeLastUses();
    public:

        bool isSynthesizedMember() const { return synthesizedMember; }
        void setSynthesizedMember(bool v) { synthesizedMember = v; }

        // True iff this method returns a `stack`-constructed value by copy
        // (lowered via the sret + NRVO ABI). Computed lazily by scanning the
        // body for a `return stack X(...)`; cached. See returnsStackValueCache.
        bool returnsStackValue();

        // Shared body-scan helpers, exposed for lambdas (M5(b)) which run the
        // same "does this body return by stack value?" question over their
        // expression or block body to choose the sret-vs-ownership function-
        // type ABI. Definitions live in Method.cpp.
        static bool exprIsStackConstruction(const ExpressionPtr& e);
        static bool nodeHasStackReturn(const AbstractSyntaxNodePtr& node);
        static bool blockHasStackReturn(const BlockPtr& block);

        // stdlib-ownership-convention U2 — true iff this method's body PROVES
        // its plain (non-`#`) result is a window into the receiver's interior:
        // every return is a `this.field` read (or an index into one), and
        // there is at least one. A plain return is NOT statically a borrow in
        // cajeta — the return flag is runtime state, so a plain-return wrapper
        // can ride an inner `#` call's title through — which is why the
        // transfer-of-a-borrow check may only fire on a PROVEN view. See the
        // commentary in Method.cpp.
        bool returnsInteriorView() const;
        static bool exprIsInteriorRead(const ExpressionPtr& e);
        static bool nodeReturnsOnlyInteriorViews(
            const AbstractSyntaxNodePtr& node, bool& sawView);

        // [heap-optional-return] lint: warn when a method declared
        // `#Optional<T>` returns only `heap Optional<...>(...)` values
        // — the heap allocation is scope-bounded (becomes the return,
        // never escapes elsewhere) and can usually be a value-return
        // `stack Optional<...>(...)` via the M3 sret/NRVO ABI. Fires
        // once per method (statically-deduped) from generateCode.
        void lintHeapOptionalReturn();

        // ---- Frame-arena eligibility (frame-arena-plan U2) ----------------
        // Names of owned String-concat locals proven non-escaping by the escape
        // pre-pass (computeArenaEligibility): they bump-allocate from the frame
        // arena and register no drop entry. usesArena() is true iff the method has
        // any such local (gates Block's mark/reset so arena-free methods pay zero).
        std::set<std::string> arenaEligibleNames;
        bool methodUsesArena = false;
        bool usesArena() const { return methodUsesArena; }
        bool isArenaEligibleLocal(const std::string& n) const {
            return arenaEligibleNames.find(n) != arenaEligibleNames.end();
        }
        // Walk the method body: collect names that escape (#-moved, returned,
        // stored/aliased) and String-concat decls; flag the non-escaping concats'
        // BinaryOpExpression nodes arena-eligible and record their names here.
        void computeArenaEligibility();

        // Push a fresh (empty) drop frame onto the stack. Block::generateCode
        // calls this at its entry; the frame collects every owned local
        // declared inside the block until the matching pop.
        void pushDropFrame() { dropFrameStack.emplace_back(); }

        // Pop the top frame without emitting any IR. Use after the
        // matching emitTopFrameDrops (or after a terminator made the
        // drops dead code).
        void popDropFrame() {
            if (!dropFrameStack.empty()) dropFrameStack.pop_back();
        }

        // Register a DropEntry alloca (as emitted by LocalVariableDeclaration for
        // an owned local) into the current top frame.
        void registerDropEntry(llvm::Value* entry) {
            if (dropFrameStack.empty()) {
                // Defensive: a caller that registers before any block has
                // pushed gets a synthetic outer frame so the entry isn't
                // silently dropped. Shouldn't happen in normal flow.
                dropFrameStack.emplace_back();
            }
            dropFrameStack.back().push_back(entry);
        }

        // Emit drop-chain pops + drops for the top frame's entries in
        // reverse order. Called by Block::generateCode at its normal
        // closing `}` to release the locals declared inside that block.
        // The frame is NOT popped — call popDropFrame() afterwards.
        void emitTopFrameDrops(CajetaModulePtr module);

        // Emit drop-chain pops + drops for EVERY frame still on the
        // stack, walking from innermost to outermost (LIFO across
        // frames, LIFO within each). Called by ReturnStatement before
        // the ret so the chain unwinds across all enclosing blocks at
        // once. Frames stay on the stack — each enclosing block will
        // observe the terminator and skip its own fire on the way out.
        void emitOwnerDrops(CajetaModulePtr module);

        // Position of the declaration's NAME token (1-based line, 0-based col).
        // Set by the visitor at each method/constructor declaration site; 0 means
        // synthesized (drop methods, mocks, template instantiations). Consumed by
        // the xref export (ide-symbol-index §2).
        // resident-debug-server 9.2: instantiated bodies are re-parsed from a
        // SYNTHETIC wrapper snippet, so their token lines are snippet-relative
        // (they merely look plausible — a long preamble puts them in range of
        // the real file). This delta maps snippet line -> true file line and
        // is derived from the template's own declLine vs the snippet's, so it
        // is independent of preamble size. 0 = ordinary (non-instantiated).
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

        // The module this method's IR is CREATED in (see emitModule). Falls back
        // to the DECLARING CLASS's emit module — a class-template instantiation
        // emitted into a user module carries every method with it — then to the
        // resolution module (production / non-reuse). Out-of-line: needs
        // CajetaClass::getEmitModule.
        CajetaModulePtr getEmitModule();
        void setEmitModule(CajetaModulePtr m) { emitModule = m; }

        const string toCanonical(bool labeled = false) {
            return buildCanonical(parent, name, parameterList, labeled);
        }

        const string toGeneric(bool labeled = false) {
            return buildGeneric(parent, name, parameterList, labeled);
        }

        // Map-storage key for `methods` / `staticMethods` / labeled+
        // unlabeled method maps. Equal to `toCanonical(labeled)` for
        // ordinary methods AND for method-templates themselves.
        //
        // For a method-template *instantiation* (both
        // methodTypeParameters and methodTypeArguments non-empty), the
        // key has the concrete type-args appended as
        // `<argCanonical,argCanonical,...>`. Two instantiations of
        // `static <T> Collector<T, ArrayList<T>> toList()` with T=int32
        // and T=String thereby get distinct keys even though their
        // value-param signatures (and so their `toCanonical()`) are
        // identical — which is what lets addMethod's duplicate-static
        // check accept both. See docs/specification/lang/templates/MethodLevelTemplate.md
        // § two-layer naming.
        //
        // resolveMethod looks up ordinary methods by their plain
        // canonical, so non-instantiations stay reachable. Instantiations
        // are produced and returned directly by tryInstantiateMethodTemplate
        // — they're never resolved via the map keys, so the suffix on
        // instantiation keys is invisible to lookup paths.
        const string getMapKey(bool labeled = false) const;

        // LLVM symbol name. Mirrors getMapKey(true) so instantiations get
        // distinct LLVM functions (no module-level symbol collision).
        // Ordinary methods get `toCanonical(true)` exactly as before;
        // only method-template instantiations carry the type-arg suffix.
        const string getLlvmSymbolName() const;

        void generatePrototype();

        void setBlock(BlockPtr block);

        // Exposed for ad-hoc body walks (e.g. MultiClassing Phase 3 v4's
        // narrow MCE pre-check for "method touches only inherited fields").
        // Read-only; codegen still drives via generateCode.
        BlockPtr getBlock() const { return block; }

        void createScope();

        void createLocalVariable(CajetaModulePtr module, FieldPtr field);

        void setLocalVariable(CajetaModulePtr module, string name, llvm::Value* value);

        virtual void generateCode();

        // Emit a thin forwarding wrapper to a C-runtime symbol named
        // `symbol`. Called by generateCode() when the method carries an
        // @Native annotation. See docs/specification/
        // "Native methods" for the user-facing contract.
        void emitNativeForwardingBody(const std::string& symbol);
        // Record a native-library requirement (lib-id, symbol) as LLVM named
        // metadata `cajeta.native.reqs` on the emit module. Consumed by .cja
        // emission (native-deps unit 3) and the resolver (unit 4). Deduped.
        void recordNativeRequirement(const std::string& lib,
                                     const std::string& symbol);

        static map<string, MethodPtr>& getArchive();

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<FormalParameterPtr> parameters, bool labeled);

        static string buildCanonical(CajetaClassPtr parent, const string& name, vector<ParameterEntry> parameters, bool labeled);

        // Template-origin canonical: same shape as `buildCanonical` but
        // un-substitutes the instantiation's typeArguments back to the
        // template's typeParameter names in the parameter list. Used to
        // produce a substitution-stable hash for wildcard / capture
        // dispatch so the lookup hash matches across all
        // instantiations of the same template (every `Box<X>` resolves
        // `set` to the same vtable hash).
        //
        // v1 scope: top-level param-type substitution only. A param of
        // type `Optional<T>` substituted to `Optional<int32>` still
        // canonicalizes as `Optional<int32>` — recursive descent into
        // generic-arg lists is a follow-on slice if dispatch through
        // wrapped-generic params surfaces a need.
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


