//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "../asn/AbstractSyntaxNode.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "../type/QualifiedName.h"
#include "../type/CajetaClass.h"
#include "../method/Method.h"
#include "CompilerMode.h"
#include "support/Any.h"
#include <string>
#include <fstream>
#include <filesystem>
#include <functional>
#include <queue>
#include <set>
#include <llvm/Support/raw_os_ostream.h>
#include "../type/ScopeStack.h"

using namespace std;
using std::ofstream;

#define PATH_SEPARATOR              '/'
#define PACKAGE_SEPARATOR           '.'
#define CAJETA_EXTENSION            ".cajeta"
#define CAJETA_IR_EXTENSION         ".ll"

namespace cajeta {
    class StructureMetadata;
    typedef shared_ptr<StructureMetadata> StructureMetadataPtr;

    class CajetaClass;

    class CajetaModule : public enable_shared_from_this<CajetaModule> {
    public:
        // TBAA access kind (see the TBAA section further down for rationale).
        // Declared at the top so the private provenance map can name it.
        enum class TbaaKind { None, Char, Field, ArrayElem };

    private:
        // thread-safe-compiler Unit 3: per-compile registries are thread_local
        // (concurrent compiles never share them). stdlibModule/reuseEmitModule/
        // reuseEpoch stay shared — the frozen-stdlib tier handled in Units 5-6.
        static thread_local map<string, MethodPtr> methods;
        static thread_local map<string, CajetaModulePtr> strutureToModule;
        static thread_local map<string, CajetaModulePtr> moduleVariables;
        // Classes annotated `@Aspect`, in declaration order across all
        // modules in the compile. AspectModel.md § Implementation
        // roadmap A2: pointcut matching (A3) walks this list to find
        // candidate aspects for each user method. Process-global so an
        // aspect declared in one module can advise methods declared in
        // another — matches the "compiler scans all source" model the
        // doc's DI graph already uses. Cleared on each fresh Compiler
        // by resetGlobals.
        static thread_local vector<CajetaClassPtr> aspectClasses;

    public:
        // Component registry (AspectModel.md § A8). Holds every class
        // annotated `@Component`, `@Repository`, or `@TestComponent`
        // across all modules. Populated in lockstep with parsing —
        // visitClassDeclaration registers the class right after the
        // annotation-instance capture loop runs. resolveDependencyGraph
        // walks this list to filter by profile, apply test overrides,
        // and validate the DI graph.
        //
        // The ComponentDescriptor is a parse-time DTO that holds the
        // already-extracted DI metadata so the resolver doesn't have
        // to re-walk every annotation. `name` and `profiles` come from
        // the @Component / @Profile annotations on the class; the
        // `isTestComponent` flag distinguishes @TestComponent
        // declarations so the resolver can drop them outside test
        // compilations and prefer them inside.
        struct ComponentDescriptor;
        typedef shared_ptr<ComponentDescriptor> ComponentDescriptorPtr;

        // @Factory descriptor (AspectModel.md § @Factory, R1–R4). A
        // @Factory class holds one provider method per provided type.
        // The descriptor is the parse-time model the DI graph consumes:
        // resolution keys on the method *signature* (R1), so discovery
        // captures the return type (= provided type), the @Inject params
        // (dependency edges), the assisted (unmarked) params, and the
        // method scope (@Singleton / @Transient, R4) — never the body.
        struct FactoryDescriptor;
        typedef shared_ptr<FactoryDescriptor> FactoryDescriptorPtr;

        // One resolved @Inject site on the owning component: the
        // StructureProperty being assigned and the component whose
        // singleton fills it. resolveDependencyGraph populates this
        // list as it validates; A9 codegen reads it to emit the
        // __postConstruct body without re-running lookup.
        //
        // AllocateMode follows the spec's four-mode hierarchy. The
        // default is Singleton. OwnerScope / Transient are
        // implemented in codegen as fresh-per-site allocation
        // (v1 simplification: identical at field-level granularity;
        // they diverge only at constructor-parameter sites which
        // aren't injected yet). CallScope is rejected as not-yet-
        // supported until the implicit function-body scope wires up.
        enum class AllocateMode {
            Singleton,
            OwnerScope,
            CallScope,
            Transient,
        };
        struct ResolvedDependency {
            StructurePropertyPtr field;
            ComponentDescriptorPtr target;     // component-ctor target; null if factory-provided or (optional && no candidate)
            // When the @Inject is satisfied by an all-injected @Factory
            // provider (rather than a @Component ctor), `factory` is set
            // and `providerIdx` indexes factory->providers. `target` is
            // then null. Codegen routes to the factory accessor.
            FactoryDescriptorPtr factory;
            int providerIdx = -1;
            AllocateMode allocate = AllocateMode::Singleton;
            bool optional = false;
        };

        struct ComponentDescriptor {
            CajetaClassPtr klass;
            string name;                 // "" if no name = qualifier
            vector<string> profiles;     // empty = profile-neutral
            bool isTestComponent = false;
            // Populated by resolveDependencyGraph after validation.
            // One entry per @Inject field on this component.
            vector<ResolvedDependency> resolvedFields;
            // The synthesized lazy singleton storage. Created on
            // first reference in InjectMethod codegen and shared by
            // every IR-level fetch (which is one per active call
            // site). Nullptr until the inject helper's first emit.
            llvm::GlobalVariable* singletonGlobal = nullptr;
        };

        // One provider method on a @Factory class. Each `param` is
        // either an @Inject edge (graph-resolved) or assisted
        // (caller-supplied) per R3; `hasAssisted` flips the consumer's
        // resolution from "inject the product" (provider) to "inject
        // the factory and call it" (factory-injection). `scope` is the
        // method-level @Singleton / @Transient (R4); Singleton default.
        struct FactoryProvider {
            MethodPtr method;
            CajetaTypePtr providedType;        // = the method's return type
            string name;                        // optional @Factory name qualifier ("" v1)
            struct Param {
                FormalParameterPtr param;
                bool injected = false;          // @Inject => edge; else assisted
                string nameQualifier;           // @Inject(name = "...")
                // Filled by resolveDependencyGraph (Unit 3) for @Inject
                // params: the resolved provider — a @Component ctor
                // (resolvedTarget) or another all-injected @Factory
                // provider (resolvedFactory + resolvedProviderIdx).
                ComponentDescriptorPtr resolvedTarget;
                FactoryDescriptorPtr resolvedFactory;
                int resolvedProviderIdx = -1;
            };
            vector<Param> params;
            bool hasAssisted = false;           // any assisted param => factory-injection
            AllocateMode scope = AllocateMode::Singleton;   // R4
            // The synthesized provider accessor (all-injected providers
            // only) — a static method on the factory class that builds
            // the product via the factory method. Consumers' @Inject of
            // the product route to it. Null until synthesized (Unit 4a);
            // the singleton memo storage lives on the method itself.
            MethodPtr accessor;
        };

        struct FactoryDescriptor {
            CajetaClassPtr klass;
            vector<FactoryProvider> providers;
            // The @Factory class is also registered as a plain
            // @Component so its @Inject collaborator fields resolve and
            // it is itself an injectable process singleton (R3's
            // assisted case injects the factory). This back-points to
            // that descriptor; the factory singleton storage lives on
            // selfComponent->singletonGlobal.
            ComponentDescriptorPtr selfComponent;
        };

        // Lazy-stdlib import hook. Installed by the Compiler so that an
        // `import cajeta.math.X` (or a bare reference to a hardcoded
        // cajeta.math type such as Matrix) seen during a parse triggers
        // that stdlib package's on-demand parse. Null until installed; the
        // installed hook is a no-op for non-lazy packages. Lives here (not
        // on Compiler) so the resolver / onImportDeclaration can fire it
        // without a Compiler dependency. See Compiler's lazy stdlib loader.
        // Per-compile (each thread installs its own during setup); thread_local
        // so concurrent compiles don't race on the std::function.
        static thread_local std::function<void(const std::string&)> stdlibImportHook;

        // On-demand USER-class materialization (table-fit spec §2): given a
        // canonical name, compile its declaring module NOW so the caller sees
        // the real declaration (record flag, fields) instead of a placeholder.
        // Installed by Compiler beside the import hook; lives here for the
        // same no-Compiler-dependency reason. Returns true when a module was
        // compiled. Null until installed; callers must null-check.
        static thread_local std::function<bool(const std::string&)> userMaterializeHook;
    private:
        static thread_local vector<ComponentDescriptorPtr> componentClasses;
        static thread_local vector<FactoryDescriptorPtr> factoryClasses;

        // The profile name passed to the compiler (`--profile=<name>`)
        // or set by the test driver. Default `"prod"`; JIT test helper
        // sets `"test"`. Resolved at graph-build time — a component's
        // @Profile annotations must include this value, or carry no
        // @Profile at all, to participate.
        static thread_local string activeProfile;

        // The module currently being walked (parse pass or template-
        // instantiation walk). Used as a fallback by call sites that don't
        // thread `module` through their APIs — notably the parse-time
        // Expression / Type construction path, which is several layers deep
        // off the visitor. Set by `Compiler::parse` and by
        // `CajetaClass::instantiate` around their respective walks.
        // Single-threaded compilation, so a plain pointer is enough; if we
        // ever parallelize parsing this becomes thread_local.
        static thread_local CajetaModulePtr activeModule;

        // Incremental compilation (Phase 2/3): the module whose method body
        // is currently being lowered by Method::generateCode (innermost frame
        // — set via an RAII guard, so nested codegen restores correctly).
        // Null outside codegen (e.g. during parse). The template-instantiation
        // choke point reads this to attribute a cross-module instantiation to
        // the module that triggered it. Single-threaded codegen, like
        // activeModule.
        static thread_local CajetaModulePtr currentCodegenModule;

        // Test-reuse fallback emit target. The harness sets this to the per-test
        // primary USER module before compiling, so a stdlib-template
        // instantiation triggered with no user-type arg and no active codegen
        // frame (e.g. during type resolution) still emits into a disposable user
        // module rather than the cached stdlib. Null outside the reuse path.
        static thread_local CajetaModulePtr reuseEmitModule;
        // The llvm::Module of the function currently being emitted. Set (RAII)
        // by Method::generateCode / clinit body lowering; read by
        // emitTargetLlvmModule() so IR-creation helpers land new IR in the emit
        // module without swapping `module` (which would split per-module state).
        static thread_local llvm::Module* currentEmitLlvmModule;
        static thread_local uint64_t reuseEpoch;

        // The compiler-owned module that holds the parsed stdlib
        // (cajeta.error.* today) and the linked runtime bitcode.
        // Set by Compiler's ctor, cleared in resetGlobals. User
        // modules don't carry their own stdlib copy — references
        // to stdlib classes and runtime functions resolve through
        // module-local extern declarations whose definitions live
        // here, and the JIT/AOT merge unifies them.
        static thread_local CajetaModulePtr stdlibModule;


        map<string, map<string, QualifiedNamePtr>> imports;
        QualifiedNamePtr qName;
        string sourcePath;
        bool scriptUnit = false;
        std::set<string> scriptBindingNames;
        string currentSourceFile_;   // see currentSourceFile()
        string sourceRoot;
        string archiveRoot;
        string archivePath;

        map<string, CajetaClassPtr> structures;
        bool lambdaClassPtrReturn = false;
        MethodPtr currentMethod;
        StructureMetadataPtr structureMetadata;

        // Incremental compilation (docs/IncrementalCompilation.md,
        // Phase 2): the set of template instantiations this module's codegen
        // drove into ANOTHER module (typically stdlib) — e.g. a method body
        // calling `xs.stream()` instantiates `ArrayStream<int32>` into the
        // stdlib module. If this module's codegen is later skipped (clean in
        // an incremental build), those instantiations would vanish unless
        // replayed, so we record them as "obligations". Sorted set → stable
        // sidecar ordering. Same-module instantiations are NOT recorded (they
        // travel with the module's own IR).
        std::set<std::string> instantiationObligations;

        // Incremental compilation (Phase 3): manifest-designated state for
        // this module. A clean module is parsed for declarations (+ Phase-1
        // prototypes) but codegen-skipped: no generateCode, no reflect-body /
        // clinit / sidecar emission; at emit time its llvm::Module is
        // REPLACED by the cached `.bc` from cacheBcSlot, and the obligations
        // recorded at cacheObligationsSlot are replayed into stdlib before
        // the codegen loop. Slots are also set on DIRTY modules (write
        // targets); both empty when no manifest is active.
        bool incrementalClean = false;
        string cacheBcSlot;
        string cacheObligationsSlot;
        string cacheObjSlot;   // optional (Phase 6-alt .o cache)

        // Compiler-level options that codegen consults. Set on the module by the
        // Compiler at creation time (so each module produces IR consistent with the
        // current invocation's CLI flags). The CompilerFlags struct (docs/
        // CompilerModes.md) is the authoritative store; getFlags() / setFlags()
        // are the new accessors. boundsCheckEnabled is a backward-compat shim.
        CompilerFlags compilerFlags = CompilerFlags::defaultsForMode(CompilerMode::Debug);

        // Lean linker / DCE keep-set (plans/compiler/lean-linker-dce.md). The
        // whole-program set of class canonical names whose reflection
        // registration ctor must still be emitted under LinkMode::Lean.
        // Shared across every module of a single compile (it is a whole-program
        // decision), set by the Compiler after resolveDependencyGraph and before
        // codegen. Null in Open mode / JIT (no gating); when null, keepsClass()
        // keeps everything. Step 0a populates it with ALL classes (inert); 0b
        // narrows it via reachability.
        std::shared_ptr<const std::set<std::string>> keepSet;

        // Cache of interned source-file `const char*` globals — populated on
        // demand by getOrCreateSourceFileConstant. One entry per source
        // path emitted into this module; reused across all chain-push
        // codegen sites that need to tag with the same file.
        std::map<std::string, llvm::Constant*> sourceFileConstants;

    public:
        // Active loop targets for break/continue. Each loop pushes its targets on
        // entry to its body codegen and pops on exit; break/continue read the
        // innermost frame. The optional label lets labeled break/continue
        // (`break outer;`) find the matching loop without walking from the
        // innermost — set by the surrounding IdentifierLabel statement (via
        // setPendingLoopLabel) which the loop consumes on push.
        struct LoopContext {
            llvm::BasicBlock* continueTarget;
            llvm::BasicBlock* breakTarget;
            std::string label;
            // C1 follow-up: the tryFinallyStack depth at loop entry. A break/
            // continue out of one or more try bodies inside the loop must run
            // those tries' finallys (and pop their frames) down to this watermark,
            // just like a return does for all enclosing tries.
            size_t tryFinallyDepth;
        };

    private:
        std::vector<LoopContext> loopContextStack;
        // Label set by the most recent unconsumed IdentifierLabel — the
        // immediately-following loop's pushLoopContext picks it up. Cleared
        // after consumption so unrelated nested loops don't inherit it.
        std::string pendingLoopLabel;

        // Stack of currently-active try-blocks' catch types. Pushed at the
        // start of TryStatement's body codegen, popped before the catch
        // body's codegen begins (so a throw inside a catch handler isn't
        // considered caught by the same try's clauses). Each frame is the
        // list of catch types declared on that try. Consulted by the
        // uncaught-throws lint at call sites (#209) to suppress warnings
        // when an enclosing try would catch the throw.
        std::vector<std::vector<CajetaTypePtr>> tryCatchStack;

        // One entry per enclosing `try` body currently being codegen'd — the
        // finally StatementPtr (null for catch-only tries). A `return` (or
        // break/continue) escaping those try bodies must, innermost-first, emit
        // a matching `__cajeta_exc_pop` AND run each finally, or the frame leaks
        // on the runtime exception chain (its stack slot is reused and a later
        // pop reads a garbage `prev` — the HttpsServer keep-alive crash) and the
        // finally never runs (bugfix-plan C1). Pushed/popped in lockstep with the
        // try body in TryStatement::generateCode; consumed by emitTryFinallyUnwind
        // at every return/break/continue site. See pushTryFinally.
        std::vector<std::shared_ptr<void>> tryFinallyStack;

        // Type-parameter substitution stack for template instantiation. Each
        // frame is a map from parameter name (T, K, V, ...) to the concrete
        // CajetaTypePtr it resolves to during the current instantiation walk.
        // Pushed on entry to `CajetaClass::instantiate`, popped on exit.
        //
        // Only the TOP frame is consulted by type resolution — parameters
        // never leak across instantiations. When `Pair<A,B> extends Tuple<A>`
        // is materialized, `A` is substituted to `int32` *before* recursing
        // into Tuple's instantiation; Tuple's frame contains `T → int32`,
        // not Pair's `A`. v1 doesn't support nested template classes, so a
        // single-level lookup is sufficient.
        std::vector<std::map<std::string, CajetaTypePtr>> typeSubstitutionStack;

        // Current state
        ScopeStack scopeStack;
        list<CajetaClassPtr> structureStack;
        list<MethodPtr> toGenerate;
        llvm::Module* llvmModule;
        llvm::IRBuilder<>* builder;
        llvm::LLVMContext* llvmContext;
        llvm::TargetMachine* targetMachine;
        CajetaTypePtr initializerType;
        string targetTriple;

        // TBAA: provenance of array-element / field GEPs, plus the lazily-built
        // metadata nodes (see the TBAA section in the public API below).
        std::unordered_map<const llvm::Value*, TbaaKind> tbaaProvenance;
        llvm::MDNode* tbaaCharType = nullptr;
        llvm::MDNode* tbaaFieldType = nullptr;
        llvm::MDNode* tbaaArrayElemType = nullptr;
        llvm::MDNode* tbaaFieldTag = nullptr;
        llvm::MDNode* tbaaArrayElemTag = nullptr;
        llvm::MDNode* tbaaCharTag = nullptr;
        void ensureTbaaNodes();


    public:
        CajetaModule(llvm::LLVMContext* llvmContext,
            string sourcePath,
            string sourceRoot,
            string archiveRoot,
            string targetTriple,
            llvm::TargetMachine* targetMachine);

        // Synthetic-module ctor for the compiler-owned stdlib module.
        // No source path is parsed for package/module-name derivation;
        // qName is set explicitly here and re-set per-file by
        // parseStdlibInto as each stdlib source is walked.
        CajetaModule(llvm::LLVMContext* llvmContext,
            QualifiedNamePtr qName,
            string targetTriple,
            llvm::TargetMachine* targetMachine);

        QualifiedNamePtr getQName() const {
            return qName;
        }

        void setQName(QualifiedNamePtr qName) {
            CajetaModule::qName = qName;
        }

        list<CajetaClassPtr>& getStructureStack() { return structureStack; }

        llvm::LLVMContext* getLlvmContext() {
            return llvmContext;
        }

        llvm::Module* getLlvmModule() const {
            return llvmModule;
        }

        // Swap the backing llvm::Module. Used by the test stdlib-reuse path to
        // redirect codegen into a per-test clone so the cached stdlib module is
        // never mutated. Ownership of the previous module is NOT freed here —
        // the caller manages it.
        void setLlvmModule(llvm::Module* m) { llvmModule = m; }

        void setBuilder(llvm::IRBuilder<>* builder) {
            this->builder = builder;
        }

        llvm::IRBuilder<>* getBuilder() {
            return this->builder;
        }

        // ── TBAA (type-based alias analysis) metadata ──────────────────────
        // Cajeta emits coarse TBAA so the optimizer knows an array-element store
        // can't alias the enclosing object's fields (array buffers and object
        // storage are always disjoint allocations). Two disjoint scalar nodes —
        // "cajeta field" and "cajeta array element" — both descend from an
        // omnipotent-char node (which aliases everything), so a raw/byte access
        // tagged Char still conservatively aliases all. An UNtagged access is
        // conservative too. Accesses get tagged via a provenance map populated
        // at the array- and field-GEP emit sites, then applied to the consuming
        // loads/stores by applyTbaaTags(). Sound because differently-tagged
        // (field vs array-elem) memory genuinely never overlaps. (The TbaaKind
        // enum is declared at the top of the class.)

        // Record that `ptr` (an array-element or field GEP) has the given access
        // kind so applyTbaaTags() can later tag the loads/stores that use it.
        void recordTbaaProvenance(llvm::Value* ptr, TbaaKind kind);

        // Walk every defined function in this module's llvm::Module; for each
        // load/store whose pointer operand (modulo pointer casts) has recorded
        // provenance, attach the matching !tbaa access tag. Idempotent; run once
        // per module after codegen, before optimization.
        void applyTbaaTags();

        // Allocate a stack slot in the CURRENT function's ENTRY block, not at
        // the builder's current insertion point. Local-variable slots must use
        // this: an `alloca` emitted inside a loop body re-allocates native
        // stack on every iteration at runtime (it is not reclaimed until the
        // function returns), so a hot loop overflows the stack at -O0 (-O3
        // hides it via mem2reg). Entry-block allocas execute once. Falls back
        // to the current point only when there's no active function/block.
        llvm::AllocaInst* createEntryAlloca(llvm::Type* ty, const std::string& name = "");

        CajetaTypePtr getInitializerType() const;

        void setInitializerType(CajetaTypePtr initializerType);

        const string& getSourcePath() const {
            return sourcePath;
        }

        void setSourcePath(const string& sourcePath) {
            this->sourcePath = sourcePath;
        }

        // Script units (script-units spec §3.2): true when this module's source
        // was a script-shaped unit rewritten into the implicit-class form. The
        // wrapper's declared package (`cajeta.script` by default) need not
        // match the host-chosen file location, so the package/path agreement
        // check is skipped for script modules.
        void setScriptUnit(bool v) { scriptUnit = v; }
        bool isScriptUnit() const { return scriptUnit; }

        // The unit's session-binding names (script-units spec §4): top-level
        // declarations collected by the synthesis pass. Codegen promotes these
        // owners to the runtime session registry instead of the entry's drop
        // frame.
        void setScriptBindingNames(std::vector<string> names) {
            scriptBindingNames = std::set<string>(names.begin(), names.end());
        }
        bool isScriptBindingName(const string& name) const {
            return scriptBindingNames.find(name) != scriptBindingNames.end();
        }

        // The file currently being parsed INTO this module, in remapped
        // (build-root-independent) form. A user module is one file, so this is
        // just remappedSourcePath(). The stdlib is many files parsed into one
        // synthetic module with an empty source path — parseStdlibInto sets this
        // per file so each class can record where it was actually declared.
        // Without it every stdlib frame renders as `Type.method(:268)`.
        string currentSourceFile() const {
            return currentSourceFile_.empty() ? remappedSourcePath()
                                              : currentSourceFile_;
        }

        void setCurrentSourceFile(const string& file) {
            currentSourceFile_ = file;
        }

        const string& getArchiveRoot() const {
            return archiveRoot;
        }

        void setArchiveRoot(const string& archiveRoot) {
            this->archiveRoot = archiveRoot;
        }

        const string& getArchivePath() const {
            return archivePath;
        }

        void setArchivePath(const string& archivePath) {
            this->archivePath = archivePath;
        }

        map<string, map<string, QualifiedNamePtr>>& getImports() {
            return imports;
        }

        // Template-instantiation substitution stack — see field declaration
        // for semantics. Push before walking a template body with concrete
        // arguments; pop when done.
        void pushTypeSubstitution(std::map<std::string, CajetaTypePtr> frame) {
            typeSubstitutionStack.push_back(std::move(frame));
        }
        void popTypeSubstitution() {
            if (!typeSubstitutionStack.empty()) typeSubstitutionStack.pop_back();
        }
        // Returns nullptr if `name` isn't bound by the current top frame.
        // Only the top is consulted; parameters never leak across nested
        // instantiations (see field-comment rationale).
        CajetaTypePtr lookupTypeParameter(const std::string& name) const {
            if (typeSubstitutionStack.empty()) return nullptr;
            const auto& top = typeSubstitutionStack.back();
            auto it = top.find(name);
            return it == top.end() ? nullptr : it->second;
        }
        // Returns a pointer to the top frame (or nullptr if the stack is
        // empty) so callers building a new frame can inherit bindings.
        // Read-only by intent — modify through pushTypeSubstitution.
        const std::map<std::string, CajetaTypePtr>* getCurrentTypeSubstitution() const {
            if (typeSubstitutionStack.empty()) return nullptr;
            return &typeSubstitutionStack.back();
        }

        map<string, CajetaClassPtr>& getStructures() {
            return structures;
        }

        ScopeStack& getScopeStack() {
            return scopeStack;
        }

        static map<string, CajetaModulePtr>& getStructureToModule() {
            return strutureToModule;
        }

        // DCE Tier-0b reflection-usage accumulator (lean-linker-dce.md §3.2).
        // Reset per compile via resetReflectionKeep (stdlib-prime cache reuses
        // the process). forcesAll ⇒ keep-all; sites ⇒ narrow contributions the
        // Compiler resolves against the full canonicalMap after quiescence.
        struct ReflSite {
            enum Kind { BoundClosure, ForNameLiteral, PackageLiteral, Annotated,
                        MethodAnnotated };
            Kind kind;
            std::string selector;  // T canonical / class name / package / anno short
        };
        struct ReflectionKeep {
            bool forcesAll = false;
            std::vector<ReflSite> sites;
            // 0c classifier: human-readable reason for each forces-ALL site, so a
            // lean build can warn and suggest a tighter selector.
            std::vector<std::string> forceAllReasons;
        };
        static ReflectionKeep& reflectionKeep() {
            static thread_local ReflectionKeep instance;  // per-compile (U3)
            return instance;
        }
        static void resetReflectionKeep() { reflectionKeep() = ReflectionKeep(); }
        static void noteForceAll(const std::string& reason) {
            auto& k = reflectionKeep();
            k.forcesAll = true;
            k.forceAllReasons.push_back(reason);
        }

        // Aspect registry — see the aspectClasses field. Callers that
        // walk it (A3 pointcut matching) take the list as it stands at
        // codegen time; aspect declarations all land during the parse
        // phase, before any per-method codegen runs.
        static void registerAspectClass(CajetaClassPtr klass) {
            aspectClasses.push_back(std::move(klass));
        }
        static const vector<CajetaClassPtr>& getAspectClasses() {
            return aspectClasses;
        }

        // A3 pointcut-matching pass. Run once after every module's
        // parse completes and before any method's codegen begins —
        // i.e., between Compiler::compile(module) and Phase 1 in
        // Compiler::compile(entryMethod, ...). For each advice
        // method on each registered aspect, resolve its pointcut
        // class argument, classify it as marker-annotation or type-
        // based, walk every user method, and push an AdviceMatch
        // onto the ones that match. A4+ reads these matches at
        // codegen.
        static void resolveAdviceMatches();

        // Component registry (AspectModel.md § A8). Mirrors the
        // aspect registry shape — registerComponent is called from
        // visitClassDeclaration once per @Component / @Repository /
        // @TestComponent class; resolveDependencyGraph walks the
        // list at validate-time.
        static void registerComponent(ComponentDescriptorPtr c) {
            componentClasses.push_back(std::move(c));
        }
        static const vector<ComponentDescriptorPtr>& getComponentClasses() {
            return componentClasses;
        }

        // @Factory registry. registerFactory is called from
        // visitClassDeclaration once per @Factory class (after the
        // provider methods are described); resolveDependencyGraph walks
        // both registries so @Inject resolves to either a @Component
        // ctor or a @Factory provider, with a both-provide ambiguity.
        static void registerFactory(FactoryDescriptorPtr f) {
            factoryClasses.push_back(std::move(f));
        }
        static const vector<FactoryDescriptorPtr>& getFactoryClasses() {
            return factoryClasses;
        }

        // Active profile selector for component filtering. Default
        // "prod"; CLI flag `--profile=<name>` (compile entry) and the
        // test driver (JitTestHelper) override before
        // resolveDependencyGraph runs.
        static const string& getActiveProfile() { return activeProfile; }
        static void setActiveProfile(const string& p) { activeProfile = p; }

        // DI graph validation pass (AspectModel.md § A8). Filter
        // componentClasses by activeProfile, apply @TestComponent
        // overrides, walk each surviving component's @Inject fields,
        // and validate the graph. Throws Exception on missing
        // implementation, circular dependency, or ambiguous
        // resolution. Codegen (A9) reads the validated graph state
        // populated by this pass.
        static void resolveDependencyGraph();

        // Drive the deferred-prototype machinery to fixed point. Walk
        // every CajetaClass in canonicalMap; for any that's not yet
        // prototypeBuilt and whose superclasses / implemented
        // interfaces are all non-placeholder (so layout is well-
        // defined), call generatePrototype. Repeat until no new
        // class became eligible — by then every class with a
        // resolvable inheritance chain is laid out, and any that
        // couldn't was waiting on a never-filled placeholder
        // (validatePlaceholders flags those).
        //
        // This runs after every module's parse and after the placeholder-
        // validation sweep, between parse and Phase 1.
        static void buildPendingPrototypes();

        // Re-run per-(class, iface) vtable synthesis for classes that
        // skipped a then-placeholder interface (lazy-package drain order).
        // Idempotent; every codegen fixed-point loop calls it per pass,
        // before method bodies emit. Implemented in CajetaModule.cpp.
        void completePendingInterfaceVTables();

        // Post-parse validation: scan canonicalMap for any
        // CajetaClass with placeholderFlag still set. A placeholder
        // marks a name that fromContext synthesized when the archive
        // vouched for it but visitClassDeclaration never arrived to
        // fill it in — meaning the pre-scan disagreed with the
        // actual parse (a name the archive saw but the visitor
        // missed). Defense-in-depth; under normal flow this finds
        // nothing.
        static void validatePlaceholders();

        // Cross-module call helper. Returns a Function* safe to use
        // as a CallInst callee inserted via `builder` — if the
        // original Function lives in the same llvm::Module as
        // `builder`'s insert point, that pointer is returned
        // directly; otherwise the helper calls getOrInsertFunction
        // on the calling module so an extern declaration appears
        // there, and returns THAT decl. Both definitions resolve to
        // the same symbol at JIT/link time, but the calling
        // module's IR carries a proper module-local reference
        // instead of a dangling cross-module Function* (which
        // Linker::linkModules can't reconcile and verifyModule
        // rejects).
        //
        // Callers pass a non-null original Function and the active
        // IRBuilder; the helper handles the case where original is
        // already declaration-only (no body) the same way — the
        // calling module gets its own declaration.
        static llvm::Function* ensureFunctionVisible(
            llvm::IRBuilder<>* builder,
            llvm::Function* original,
            llvm::FunctionType* fnType);

        // Module-targeted variants for cross-module references in
        // CONSTANT contexts (vtable / RTTI initializers built outside
        // any function body). The IRBuilder-based ensureFunctionVisible
        // can't help here — there is no insertion point.
        //
        // ensureFunctionInModule: if `original` already lives in
        // `targetModule`, return it; otherwise insert a module-local
        // declaration with the same name + signature and return that.
        // ensureGlobalInModule: same pattern for llvm::GlobalVariable
        // (vtable / RTTI globals defined on a foreign module).
        //
        // The merge step (Linker::linkModules) collapses these extern
        // decls with the real definitions when the donor module is
        // pulled into primary, so the resulting IR has a single
        // resolved symbol — no `<badref>` placeholders.
        static llvm::Function* ensureFunctionInModule(
            llvm::Module* targetModule,
            llvm::Function* original);
        static llvm::Constant* ensureGlobalInModule(
            llvm::Module* targetModule,
            llvm::GlobalVariable* original);

        // Active-module accessor. Returns the module currently being walked,
        // or nullptr outside any walk. Call sites that didn't thread a module
        // parameter through (parse-time Expression / Type construction) read
        // this. See the activeModule field for set/clear discipline.
        static CajetaModulePtr getActiveModule() { return activeModule; }
        static void setActiveModule(CajetaModulePtr m) { activeModule = m; }

        static CajetaModulePtr getCurrentCodegenModule() { return currentCodegenModule; }
        static void setCurrentCodegenModule(CajetaModulePtr m) { currentCodegenModule = m; }

        static CajetaModulePtr getReuseEmitModule() { return reuseEmitModule; }
        static void setReuseEmitModule(CajetaModulePtr m) { reuseEmitModule = m; }

        // Per-test generation counter for the reuse path. Bumped by the harness
        // (StdlibReuseCache::restoreBaseline) before each test. Caches keyed on
        // persistent stdlib objects (e.g. Method::methodInstantiationCache, which
        // lives on the stdlib template Method and would otherwise hand back a
        // prior test's instantiation pointing at a freed user module) compare
        // their stored epoch to this and self-invalidate on mismatch.
        static uint64_t getReuseEpoch() { return reuseEpoch; }
        static void bumpReuseEpoch() { ++reuseEpoch; }

        static llvm::Module* getCurrentEmitLlvmModule() { return currentEmitLlvmModule; }
        static void setCurrentEmitLlvmModule(llvm::Module* m) { currentEmitLlvmModule = m; }

        // Per-module debug-loc id range (resident-debug-server 3.2.1).
        // Assigned by the JIT host before codegen from a name-keyed,
        // append-only registry so unchanged modules keep their bases across
        // edits (stable -g IR = pooled objects survive). -1 = unranged: the
        // AOT/lint paths keep the dense global allocator.
        static constexpr int32_t kDbgLocRange = 1 << 20;
        int32_t dbgLocBase = -1;
        int32_t dbgLocUsed = 0;
        int32_t takeDbgLocId() {
            if (dbgLocBase < 0 || dbgLocUsed >= kDbgLocRange) return -1;
            return dbgLocBase + dbgLocUsed++;
        }

        static CajetaModulePtr getStdlibModule() { return stdlibModule; }
        static void setStdlibModule(CajetaModulePtr m) { stdlibModule = m; }

        static map<string, CajetaModulePtr>& getModuleVariables() {
            return moduleVariables;
        }

        // Clear cross-Compiler module/method bookkeeping. Used by Compiler's ctor so
        // each fresh Compiler instance starts with empty static state.
        static void resetGlobals();

        // Test stdlib-reuse support (mirrors CajetaType::capture/restoreBaseline).
        // captureBaseline() snapshots the module-level global registries after
        // the pristine stdlib is built; restoreBaseline() reassigns them,
        // dropping every user module's methods/structures/aspects/components and
        // re-pinning the stdlib singleton, while clearing the transient
        // active/codegen module pointers. No-ops in production.
        static void captureBaseline();
        static void restoreBaseline();
        // lint-server sibling-context reuse (spec §4): a second baseline slot
        // for "stdlib + sibling sweep", restored independently of the pristine
        // stdlib baseline on a warm request. See CajetaType's matching set.
        static void captureContextBaseline();
        static void restoreContextBaseline();
        static void invalidateContextBaseline();

        llvm::IRBuilder<>* getBuilder() const;

        bool isBoundsCheckEnabled() const {
            return compilerFlags.bounds != BoundsCheck::Off;
        }
        void setBoundsCheckEnabled(bool v) {
            compilerFlags.bounds = v ? BoundsCheck::On : BoundsCheck::Off;
        }
        const CompilerFlags& getFlags() const { return compilerFlags; }
        void setFlags(const CompilerFlags& f) { compilerFlags = f; }

        // Lean linker / DCE: share the whole-program keep-set with this module
        // (Compiler calls this on every module after computing it).
        void setKeepSet(std::shared_ptr<const std::set<std::string>> ks) {
            keepSet = std::move(ks);
        }
        // Should class `canon` (a canonical name) keep its reflection
        // registration ctor? Under Full mode — or before a keep-set is computed
        // (JIT, or pre-0b) — everything is kept. Under Lean, only keep-set
        // members. The two emission sites (StructureMetadata / CajetaClass) gate
        // appendToGlobalCtors on this. See plans/compiler/lean-linker-dce.md.
        bool keepsClass(const std::string& canon) const {
            if (compilerFlags.linkMode == LinkMode::Full) return true;
            if (!keepSet) return true;
            return keepSet->count(canon) > 0;
        }

        // Reproducible builds: the constructor seeds the module's embedded
        // source-file name with the absolute on-disk path, which would bake
        // a machine-specific string into the emitted IR. Once the compiler
        // flags (carrying --debug-prefix-map) are set, re-derive the name to
        // a machine-independent form so the same source yields byte-identical
        // bitcode across hosts. No-op for synthetic modules (empty sourcePath).
        void canonicalizeSourceFileName();

        // The module's source path in its machine-independent form — what
        // every IR-embedded path must use (source_filename, line-info frame
        // descriptors, source-tagged drop entries), so emitted IR is
        // byte-identical across build roots (docs-refactor 15.12.2).
        // Compile-time DIAGNOSTICS keep the raw absolute path (clickable).
        std::string remappedSourcePath() const {
            return remapSourcePath(sourcePath, sourceRoot,
                                   compilerFlags.debugPrefixMap);
        }

        // Pure mapping behind canonicalizeSourceFileName(), exposed for tests.
        // Applies a `--debug-prefix-map=<from>=<to>` (split on the first '=',
        // matching the GCC/Clang convention) when `sourcePath` starts with
        // <from>; otherwise falls back to a `sourceRoot`-relative path. Path
        // separators are normalized to '/' so the embedded form is stable
        // across OSes. Returns `sourcePath` unchanged only when neither the
        // map nor the root applies.
        static std::string remapSourcePath(const std::string& sourcePath,
                                           const std::string& sourceRoot,
                                           const std::string& debugPrefixMap);

        // Incremental compilation (Phase 2) — obligation capture/serialize.
        const std::set<std::string>& getInstantiationObligations() const {
            return instantiationObligations;
        }
        // Record `inst` as an obligation of `triggering` IFF it's a genuine,
        // cross-module instantiation (owned by a different module). Called
        // from codegen sites that trigger template instantiation, with
        // `triggering` = the module whose method body is being lowered.
        // No-op when same-module or either arg is null.
        static void noteCrossModuleInstantiation(
            const CajetaModulePtr& triggering, const CajetaClassPtr& inst);
        // Method-template twin of noteCrossModuleInstantiation. A method-
        // template instantiation (e.g. `Stream<int32>.map<int32>`) lands its
        // body in the HOST class's module via addMethod, separate from any
        // class-template instantiation — so replaying the class obligation
        // does NOT re-create it. Records `inst->getMapKey(false)` (which
        // carries `::`, distinguishing it from a class obligation) on
        // `triggering` IFF the host module differs. No-op when same-module or
        // either arg is null. See docs/IncrementalCompilation.md.
        static void noteCrossModuleMethodInstantiation(
            const CajetaModulePtr& triggering, const MethodPtr& inst);
        // Static-field twin (compile-cache D1). A FOLDABLE static's global is
        // defined in the DECLARING class's module only when some module's
        // codegen references it (CajetaClass::getOrCreateStaticFieldGlobal);
        // if that referencing module is later skipped as clean, nothing
        // re-demands the definition and the symbol vanishes from the fresh
        // stdlib object (undefined at link — the demand set is larger than
        // the template-obligation set). Records `Owner::field` — `::` with
        // NO `(` distinguishes it from both other kinds — on `triggering`
        // IFF the owner's module differs. No-op when same-module or any arg
        // is null/empty.
        static void noteCrossModuleStaticFieldRef(
            const CajetaModulePtr& triggering, const CajetaClassPtr& owner,
            const std::string& fieldName);
        // Write this module's obligations to a sidecar next to its emitted IR
        // (`<archiveRoot><archivePath:.ll→.obligations>`), one sorted
        // canonical name per line. No-op (and removes any stale sidecar) when
        // the set is empty, so a module that drops its last cross-module use
        // doesn't leave a misleading file behind.
        void writeObligationsSidecar() const;

        // Incremental compilation (Phase 3) — manifest-designated state.
        bool isIncrementalClean() const { return incrementalClean; }
        void setIncrementalClean(bool clean) { incrementalClean = clean; }
        const string& getCacheBcSlot() const { return cacheBcSlot; }
        const string& getCacheObligationsSlot() const {
            return cacheObligationsSlot;
        }
        void setCacheSlots(string bc, string obligations, string obj = "") {
            cacheBcSlot = std::move(bc);
            cacheObligationsSlot = std::move(obligations);
            cacheObjSlot = std::move(obj);
        }
        const string& getCacheObjSlot() const { return cacheObjSlot; }
        // Write this module's obligations to the manifest slot (unlike the
        // archive-root sidecar, ALWAYS written — explicitly empty when the
        // set is: slot absence must mean "never built", not "no obligations").
        // Returns false on I/O failure.
        bool writeObligationsToSlot() const;
        // Serialize the module's current llvm::Module to the .bc slot
        // (atomic temp+rename). Returns false on I/O failure.
        bool writeBitcodeToSlot() const;
        // Replace this module's llvm::Module with the bitcode at the .bc
        // slot, parsed into the SAME LLVMContext (the emit-time swap for a
        // clean module). Returns false (module untouched) on read/parse
        // failure — caller falls back to treating the module dirty.
        bool loadBitcodeFromSlot();

        // Intern a source-file path as a module-global constant `const char*`
        // for the debug-mode source-tagging machinery. Subsequent calls with
        // the same path return the same Constant (no duplicate globals).
        // Used by the drop-chain push-debug codegen to pass an alloc-site
        // file pointer along with the line number. Per CompilerModes.md
        // § Source-tagged drop-chain entries.
        llvm::Constant* getOrCreateSourceFileConstant(const std::string& path);

        void pushLoopContext(llvm::BasicBlock* cont, llvm::BasicBlock* brk) {
            loopContextStack.push_back(
                {cont, brk, pendingLoopLabel, tryFinallyStack.size()});
            pendingLoopLabel.clear();
        }
        // Set by IdentifierLabel; consumed by the next pushLoopContext.
        void setPendingLoopLabel(const std::string& label) {
            pendingLoopLabel = label;
        }
        // Look up a labeled loop context. Returns null if no enclosing loop
        // matches the label. Walks the stack from innermost to outermost.
        const LoopContext* findLoopContext(const std::string& label) const {
            for (auto it = loopContextStack.rbegin(); it != loopContextStack.rend(); ++it) {
                if (it->label == label) return &(*it);
            }
            return nullptr;
        }
        void popLoopContext() { if (!loopContextStack.empty()) loopContextStack.pop_back(); }
        bool hasLoopContext() const { return !loopContextStack.empty(); }
        const LoopContext& currentLoopContext() const { return loopContextStack.back(); }

        void pushTryCatchContext(std::vector<CajetaTypePtr> catchTypes) {
            tryCatchStack.push_back(std::move(catchTypes));
        }
        void popTryCatchContext() {
            if (!tryCatchStack.empty()) tryCatchStack.pop_back();
        }
        const std::vector<std::vector<CajetaTypePtr>>& getTryCatchStack() const {
            return tryCatchStack;
        }

        // Stack of enclosing try/catch bodies (during their codegen) whose
        // exception frame is currently active. A `return` must pop each frame
        // (innermost first) and run its finally before returning — otherwise the
        // frame dangles (longjmp into a dead stack) and the finally never runs
        // (bugfix-plan C1). Each entry is the finally StatementPtr (null when the
        // try has only catch clauses — just the pop is needed). Stored as
        // shared_ptr<void> to avoid a Statement.h include here; Statement.cpp
        // casts back. innermost = back().
        void pushTryFinally(std::shared_ptr<void> finallyStmt) {
            tryFinallyStack.push_back(std::move(finallyStmt));
        }
        void popTryFinally() {
            if (!tryFinallyStack.empty()) tryFinallyStack.pop_back();
        }
        std::vector<std::shared_ptr<void>>& getTryFinallyStack() {
            return tryFinallyStack;
        }
        // Detach the try-finally stack (leaving it empty) for the duration of a
        // nested function body's codegen — lambda bodies are generated inline
        // within the enclosing method's codegen, so the enclosing method's open
        // try frames must not bleed into the lambda's `return` unwind. Restore
        // the saved stack afterward.
        std::vector<std::shared_ptr<void>> takeTryFinally() {
            std::vector<std::shared_ptr<void>> saved = std::move(tryFinallyStack);
            tryFinallyStack.clear();
            return saved;
        }
        void restoreTryFinally(std::vector<std::shared_ptr<void>> saved) {
            tryFinallyStack = std::move(saved);
        }

        // Full per-function codegen-stack detach: tryFinally + tryCatch (lint)
        // + loop contexts + the pending loop label. Method bodies can be
        // generated NESTED inside another method's codegen (a method-template
        // instantiated on-reference at its call site — Method::generateCode),
        // and none of the enclosing function's control-flow stacks may bleed
        // into the nested body: a `return` there would otherwise emit the
        // CALLER's try-frame unwind (__cajeta_exc_pop with no matching push —
        // the popped frame is the caller's live try, so its later throw is
        // "uncaught"). The lambda path isolates tryFinally via takeTryFinally;
        // this is the same discipline for every per-function stack at once.
        struct FunctionCodegenStacks {
            std::vector<std::shared_ptr<void>> tryFinally;
            std::vector<std::vector<CajetaTypePtr>> tryCatch;
            std::vector<LoopContext> loops;
            std::string pendingLabel;
        };
        FunctionCodegenStacks takeFunctionCodegenStacks() {
            FunctionCodegenStacks saved;
            saved.tryFinally = std::move(tryFinallyStack);
            saved.tryCatch = std::move(tryCatchStack);
            saved.loops = std::move(loopContextStack);
            saved.pendingLabel = std::move(pendingLoopLabel);
            tryFinallyStack.clear();
            tryCatchStack.clear();
            loopContextStack.clear();
            pendingLoopLabel.clear();
            return saved;
        }
        void restoreFunctionCodegenStacks(FunctionCodegenStacks saved) {
            tryFinallyStack = std::move(saved.tryFinally);
            tryCatchStack = std::move(saved.tryCatch);
            loopContextStack = std::move(saved.loops);
            pendingLoopLabel = std::move(saved.pendingLabel);
        }

        void processMetadata(CajetaClassPtr structure);

        // Parse the embedded cajeta_runtime bitcode and Linker::linkModules-merge it
        // into this module. Idempotent — safe to call multiple times; subsequent calls
        // are no-ops. Returns true on success.
        bool linkRuntime();

        // Look up a runtime helper by name (must be linked first via linkRuntime()).
        // Returns nullptr if missing.
        //
        // `explicitTarget`: the llvm::Module the returned decl MUST be co-resident
        // with. Pass the module the IRBuilder is actually inserting into when that
        // can differ from emitTargetLlvmModule() — notably drop-wrapper bodies for a
        // plain stdlib class (emitModule unset), whose body is built into the
        // PERSISTENT stdlib module while currentEmitLlvmModule points at a per-test
        // user module. Without this the wrapper (cached in the stdlib module) would
        // call a runtime decl in a now-freed test module → cross-module reference on
        // the next reusing test. Null (default) keeps the historical
        // emitTargetLlvmModule() behavior; production / non-reuse is unaffected.
        llvm::Function* getRuntimeFunction(const std::string& name,
                                           llvm::Module* explicitTarget = nullptr);

        // The llvm::Module that IR created *right now* should land in: the module
        // of the function the builder is currently inserting into. During body
        // codegen this is where this method's Function lives — which, for a
        // stdlib-template instantiation in the test-reuse path, is the USER (emit)
        // module even though `this` is the resolution (stdlib) module. Falls back
        // to this module's own llvm::Module when the builder has no insert point
        // (e.g. constant folding before any function). In production / non-reuse
        // it always equals getLlvmModule(). Lets IR-creation helpers stay
        // emit-correct without swapping `module` (which would split per-module
        // state across the resolution and emit modules — see Method::generateCode).
        llvm::Module* emitTargetLlvmModule();

        void writeIRFileTarget() {
            string targetPath = archiveRoot + archivePath;
            std::error_code ec;

            string targetDirs = targetPath.substr(0, targetPath.rfind("/"));
            std::filesystem::create_directories(targetDirs);
            // Stack ostream (RAII): flushes + closes on scope exit. The old
            // heap-allocated raw_fd_ostream was never deleted or flushed —
            // leaking the fd and risking an unflushed/truncated .ll.
            llvm::raw_fd_ostream out(targetPath, ec, llvm::sys::fs::CD_CreateAlways);
            llvmModule->print(out, nullptr);
        }

        void setCurrentMethod(MethodPtr method) {
            this->currentMethod = method;
        }

        // 7.2.5 — true while emitting a lambda body whose synthesized
        // function returns a class POINTER (non-sret). Lambdas have no
        // Method context, so the return-flag protocol reads this instead
        // of Method::returnsClassPointer().
        void setLambdaClassPtrReturn(bool v) { lambdaClassPtrReturn = v; }
        bool isLambdaClassPtrReturn() const { return lambdaClassPtrReturn; }

        MethodPtr getCurrentMethod() {
            return currentMethod;
        }

        list<MethodPtr> getAllMethods() {
            list<MethodPtr> result;
            for (auto entry:  structures) {
                // Templates have no concrete type and their bodies reference
                // unresolved type parameters; their methods can't be lowered.
                // Concrete instantiations (e.g. Box<int32>) are codegen'd via
                // their own entries in `structures`.
                if (entry.second->isTemplate()) continue;
                for (auto methodEntry: entry.second->getMethods()) {
                    result.push_back(methodEntry.second);
                }
            }
            return result;
        }

        void onPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx);

        void onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx);

        void onStructureDeclaration(std::any any);

        static CajetaModulePtr create(llvm::LLVMContext* llvmContext,
            string sourcePath,
            string sourceRoot,
            string archiveRoot,
            string targetTriple,
            llvm::TargetMachine* targetMachine);
    };

    typedef shared_ptr<CajetaModule> CajetaModulePtr;
}

