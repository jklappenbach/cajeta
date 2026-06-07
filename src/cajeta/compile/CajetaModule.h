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
#include <queue>
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
    private:
        static map<string, MethodPtr> methods;
        static map<string, CajetaModulePtr> strutureToModule;
        static map<string, CajetaModulePtr> moduleVariables;
        // Classes annotated `@Aspect`, in declaration order across all
        // modules in the compile. AspectModel.md § Implementation
        // roadmap A2: pointcut matching (A3) walks this list to find
        // candidate aspects for each user method. Process-global so an
        // aspect declared in one module can advise methods declared in
        // another — matches the "compiler scans all source" model the
        // doc's DI graph already uses. Cleared on each fresh Compiler
        // by resetGlobals.
        static vector<CajetaClassPtr> aspectClasses;

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
            ComponentDescriptorPtr target;     // null iff `optional` and no candidate
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
    private:
        static vector<ComponentDescriptorPtr> componentClasses;

        // The profile name passed to the compiler (`--profile=<name>`)
        // or set by the test driver. Default `"prod"`; JIT test helper
        // sets `"test"`. Resolved at graph-build time — a component's
        // @Profile annotations must include this value, or carry no
        // @Profile at all, to participate.
        static string activeProfile;

        // The module currently being walked (parse pass or template-
        // instantiation walk). Used as a fallback by call sites that don't
        // thread `module` through their APIs — notably the parse-time
        // Expression / Type construction path, which is several layers deep
        // off the visitor. Set by `Compiler::parse` and by
        // `CajetaClass::instantiate` around their respective walks.
        // Single-threaded compilation, so a plain pointer is enough; if we
        // ever parallelize parsing this becomes thread_local.
        static CajetaModulePtr activeModule;

        // The compiler-owned module that holds the parsed stdlib
        // (cajeta.error.* today) and the linked runtime bitcode.
        // Set by Compiler's ctor, cleared in resetGlobals. User
        // modules don't carry their own stdlib copy — references
        // to stdlib classes and runtime functions resolve through
        // module-local extern declarations whose definitions live
        // here, and the JIT/AOT merge unifies them.
        static CajetaModulePtr stdlibModule;


        map<string, map<string, QualifiedNamePtr>> imports;
        QualifiedNamePtr qName;
        string sourcePath;
        string sourceRoot;
        string archiveRoot;
        string archivePath;

        map<string, CajetaClassPtr> structures;
        MethodPtr currentMethod;
        StructureMetadataPtr structureMetadata;

        // Compiler-level options that codegen consults. Set on the module by the
        // Compiler at creation time (so each module produces IR consistent with the
        // current invocation's CLI flags). The CompilerFlags struct (cajeta-docs/
        // CompilerModes.md) is the authoritative store; getFlags() / setFlags()
        // are the new accessors. boundsCheckEnabled is a backward-compat shim.
        CompilerFlags compilerFlags = CompilerFlags::defaultsForMode(CompilerMode::Debug);

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

        void setBuilder(llvm::IRBuilder<>* builder) {
            this->builder = builder;
        }

        llvm::IRBuilder<>* getBuilder() {
            return this->builder;
        }

        CajetaTypePtr getInitializerType() const;

        void setInitializerType(CajetaTypePtr initializerType);

        const string& getSourcePath() const {
            return sourcePath;
        }

        void setSourcePath(const string& sourcePath) {
            this->sourcePath = sourcePath;
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

        static CajetaModulePtr getStdlibModule() { return stdlibModule; }
        static void setStdlibModule(CajetaModulePtr m) { stdlibModule = m; }

        static map<string, CajetaModulePtr>& getModuleVariables() {
            return moduleVariables;
        }

        // Clear cross-Compiler module/method bookkeeping. Used by Compiler's ctor so
        // each fresh Compiler instance starts with empty static state.
        static void resetGlobals();

        llvm::IRBuilder<>* getBuilder() const;

        bool isBoundsCheckEnabled() const {
            return compilerFlags.bounds != BoundsCheck::Off;
        }
        void setBoundsCheckEnabled(bool v) {
            compilerFlags.bounds = v ? BoundsCheck::On : BoundsCheck::Off;
        }
        const CompilerFlags& getFlags() const { return compilerFlags; }
        void setFlags(const CompilerFlags& f) { compilerFlags = f; }

        // Reproducible builds: the constructor seeds the module's embedded
        // source-file name with the absolute on-disk path, which would bake
        // a machine-specific string into the emitted IR. Once the compiler
        // flags (carrying --debug-prefix-map) are set, re-derive the name to
        // a machine-independent form so the same source yields byte-identical
        // bitcode across hosts. No-op for synthetic modules (empty sourcePath).
        void canonicalizeSourceFileName();

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

        void processMetadata(CajetaClassPtr structure);

        // Parse the embedded cajeta_runtime bitcode and Linker::linkModules-merge it
        // into this module. Idempotent — safe to call multiple times; subsequent calls
        // are no-ops. Returns true on success.
        bool linkRuntime();

        // Look up a runtime helper by name (must be linked first via linkRuntime()).
        // Returns nullptr if missing.
        llvm::Function* getRuntimeFunction(const std::string& name);

        void writeIRFileTarget() {
            string targetPath = archiveRoot + archivePath;
            std::error_code ec;

            string targetDirs = targetPath.substr(0, targetPath.rfind("/"));
            std::filesystem::create_directories(targetDirs);
            llvm::raw_ostream* out = new llvm::raw_fd_ostream(targetPath, ec, llvm::sys::fs::CD_CreateAlways);
            printf("\n\n");
            llvmModule->print(llvm::outs(), nullptr);
            llvmModule->print(*out, nullptr);
        }

        void setCurrentMethod(MethodPtr method) {
            this->currentMethod = method;
        }

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

