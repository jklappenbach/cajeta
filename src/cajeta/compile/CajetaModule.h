//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "../asn/AbstractSyntaxNode.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Target/TargetMachine.h"
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

#include "ScriptLineMap.h"

namespace cajeta {
    class StructureMetadata;
    typedef shared_ptr<StructureMetadata> StructureMetadataPtr;

    class CajetaClass;
    class SessionState;

    class CajetaModule : public enable_shared_from_this<CajetaModule> {
    public:
        // TBAA access kind; named here so the private provenance map can use it.
        enum class TbaaKind { None, Char, Field, ArrayElem };

    private:
        // Per-compile registries; stdlibModule / reuseEmitModule / reuseEpoch are shared.
        static thread_local map<string, MethodPtr> methods;
        static thread_local map<string, CajetaModulePtr> strutureToModule;
        static thread_local map<string, CajetaModulePtr> moduleVariables;
        // Every `@Aspect` class in declaration order; cleared by resetGlobals.
        static thread_local vector<CajetaClassPtr> aspectClasses;

    public:
        // Every @Component / @Repository / @TestComponent class. The descriptor
        // is a parse-time DTO of the DI metadata the resolver consumes.
        struct ComponentDescriptor;
        typedef shared_ptr<ComponentDescriptor> ComponentDescriptorPtr;

        // A @Factory's parse-time model: one provider per provided type, keyed on
        // the method signature — return type, @Inject edges, assisted params.
        struct FactoryDescriptor;
        typedef shared_ptr<FactoryDescriptor> FactoryDescriptorPtr;

        // One resolved @Inject site: the property and what fills it. AllocateMode
        // defaults to Singleton; CallScope is rejected as not yet supported.
        enum class AllocateMode {
            Singleton,
            OwnerScope,
            CallScope,
            Transient,
        };
        struct ResolvedDependency {
            StructurePropertyPtr field;
            ComponentDescriptorPtr target;     // component-ctor target; null if factory-provided or (optional && no candidate)
            // Set when a @Factory provider satisfies the @Inject instead of a
            // component ctor: `target` is null and providerIdx indexes providers.
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
            // One entry per @Inject field, filled by resolveDependencyGraph.
            vector<ResolvedDependency> resolvedFields;
            // Lazy singleton storage, created on the inject helper's first emit.
            llvm::GlobalVariable* singletonGlobal = nullptr;
        };

        // One provider method on a @Factory. `hasAssisted` (any caller-supplied
        // param) flips the consumer from injecting the product to the factory.
        struct FactoryProvider {
            MethodPtr method;
            CajetaTypePtr providedType;        // = the method's return type
            string name;                        // optional @Factory name qualifier ("" v1)
            struct Param {
                FormalParameterPtr param;
                bool injected = false;          // @Inject => edge; else assisted
                string nameQualifier;           // @Inject(name = "...")
                // Filled by resolveDependencyGraph: the resolved ctor or provider.
                ComponentDescriptorPtr resolvedTarget;
                FactoryDescriptorPtr resolvedFactory;
                int resolvedProviderIdx = -1;
            };
            vector<Param> params;
            bool hasAssisted = false;           // any assisted param => factory-injection
            AllocateMode scope = AllocateMode::Singleton;   // R4
            // The synthesized static accessor that builds the product; null until
            // it is synthesized.
            MethodPtr accessor;
        };

        struct FactoryDescriptor {
            CajetaClassPtr klass;
            vector<FactoryProvider> providers;
            // The @Factory's own @Component descriptor; its singleton lives there.
            ComponentDescriptorPtr selfComponent;
        };

        // Lazy-stdlib import hook: an `import cajeta.math.X` during a parse
        // triggers that package's on-demand parse. Null until installed.
        static thread_local std::function<void(const std::string&)> stdlibImportHook;

        // On-demand USER-class materialization: compile a canonical name's module
        // NOW so callers see the real declaration. Null until installed.
        static thread_local std::function<bool(const std::string&)> userMaterializeHook;
    private:
        static thread_local vector<ComponentDescriptorPtr> componentClasses;
        static thread_local vector<FactoryDescriptorPtr> factoryClasses;

        // Profile a component's @Profile must name to participate; "prod" default.
        static thread_local string activeProfile;

        // The module currently being walked, for call sites that cannot thread one
        // through — notably parse-time Expression / Type construction.
        static thread_local CajetaModulePtr activeModule;

        // The module whose method body is being lowered (RAII-set, innermost
        // frame); null outside codegen. Attributes cross-module instantiations.
        static thread_local CajetaModulePtr currentCodegenModule;

        // Test-reuse fallback emit target, so an instantiation with no better
        // target lands in a disposable user module rather than the cached stdlib.
        static thread_local CajetaModulePtr reuseEmitModule;

        // The unit being compiled, for the SESSION emit policy. Not
        // reuseEmitModule: only already user-typed specializations route here.
        static thread_local CajetaModulePtr activeUnitModule;
        // The llvm::Module of the function being emitted; read by emitTargetLlvmModule().
        static thread_local llvm::Module* currentEmitLlvmModule;
        static thread_local uint64_t reuseEpoch;

        // The compiler-owned module holding the parsed stdlib and the linked
        // runtime bitcode; user modules reach both through extern declarations.
        static thread_local CajetaModulePtr stdlibModule;


        map<string, map<string, QualifiedNamePtr>> imports;
        QualifiedNamePtr qName;
        string sourcePath;
        bool scriptUnit = false;
        std::set<string> scriptBindingNames;
        bool scriptRootBlockPending = false;
        bool scriptEntryTopLevel = false;
        // The session this unit compiles into and the host's name for its source
        // (diagnostics); null / empty outside a session compile.
        SessionState* sessionState = nullptr;
        string scriptHostName;
        // Wrapper→host line spans, and the statement's host line for diagnostics.
        ScriptLineMap scriptLineMap;
        int scriptCurrentHostLine = 0;
        // The unit RESULT (Out[N]): whether synthesis appended the trailing
        // `return 0;`, and whether the cell's last statement is the candidate.
        bool scriptSyntheticTail = false;
        bool scriptResultPending = false;
        string currentSourceFile_;   // see currentSourceFile()
        string sourceRoot;
        string archiveRoot;
        string archivePath;
        // True for modules re-parsed from a classpath `.cja`. Source-hygiene
        // checks demote to notes: the consumer cannot edit a released archive.
        bool classpathOrigin = false;

        bool resolutionOnly = false;
        map<string, CajetaClassPtr> structures;
        bool lambdaClassPtrReturn = false;
        MethodPtr currentMethod;
        StructureMetadataPtr structureMetadata;

        // Template instantiations this module's codegen drove into ANOTHER module;
        // a skipped (clean) module replays them, since they do not ride its IR.
        std::set<std::string> instantiationObligations;

        // Manifest incremental state: a clean module is parsed but codegen-skipped,
        // its llvm::Module replaced from cacheBcSlot and obligations replayed.
        bool incrementalClean = false;
        string cacheBcSlot;
        string cacheObligationsSlot;
        string cacheObjSlot;   // optional (Phase 6-alt .o cache)

        // The invocation's CLI flags; codegen consults them through getFlags().
        CompilerFlags compilerFlags = CompilerFlags::defaultsForMode(CompilerMode::Debug);

        // Whole-program set of classes whose reflection ctor is still emitted under
        // LinkMode::Lean. Null in Open mode / JIT, where keepsClass keeps all.
        std::shared_ptr<const std::set<std::string>> keepSet;

        // Interned source-file `const char*` globals, one per path in this module.
        std::map<std::string, llvm::Constant*> sourceFileConstants;

    public:
        // Active break/continue targets, pushed per loop body. The optional label
        // (from the enclosing IdentifierLabel) lets `break outer;` find its loop.
        struct LoopContext {
            llvm::BasicBlock* continueTarget;
            llvm::BasicBlock* breakTarget;
            std::string label;
            // tryFinallyStack depth at loop entry: a break/continue out of try
            // bodies must run their finallys down to this watermark.
            size_t tryFinallyDepth;
            // dropFrameStack depth at loop entry. A break/continue skips the
            // end-of-block pop_runs of blocks opened inside the loop, so the jump
            // site must emit pop_run for every frame deeper than this.
            size_t dropFrameDepth;
        };

    private:
        std::vector<LoopContext> loopContextStack;
        // Set by the last unconsumed IdentifierLabel; the next loop consumes it.
        std::string pendingLoopLabel;

        // Catch types of the active try bodies, popped before a catch body runs so
        // a throw inside a handler is not caught by its own try. Read by the lint.
        std::vector<std::vector<CajetaTypePtr>> tryCatchStack;

        // One entry per enclosing try body being codegen'd — its finally, or null
        // for catch-only. A return/break/continue escaping them must emit
        // `__cajeta_exc_pop` and run each finally, innermost first, or the frame leaks.
        std::vector<std::shared_ptr<void>> tryFinallyStack;

        // Type-parameter frames pushed by CajetaClass::instantiate. Only the TOP is
        // consulted: arguments are substituted before recursing, so none leaks.
        std::vector<std::map<std::string, CajetaTypePtr>> typeSubstitutionStack;

        // Current state
        ScopeStack scopeStack;
        list<CajetaClassPtr> structureStack;
        list<MethodPtr> toGenerate;
        llvm::Module* llvmModule;
        // Null until this module's codegen begins (setBuilder); neither ctor
        // assigns it, so callers can tell a never-generated module apart.
        llvm::IRBuilder<>* builder = nullptr;
        llvm::LLVMContext* llvmContext;
        llvm::TargetMachine* targetMachine;
        CajetaTypePtr initializerType;
        string targetTriple;

        // TBAA provenance of array-element / field GEPs, and its metadata nodes.
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

        // Synthetic-module ctor for the compiler-owned stdlib module: no source
        // path to derive from, so qName is set here and re-set per parsed file.
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

        // Swap the backing llvm::Module (the test stdlib-reuse path redirects
        // codegen into a per-test clone). The previous module is NOT freed.
        void setLlvmModule(llvm::Module* m) { llvmModule = m; }

        void setBuilder(llvm::IRBuilder<>* builder) {
            this->builder = builder;
        }

        /** Which fused int8 dot units the target has — tier selection only, so a
         *  wrong answer costs speed, never results. Each checks the TRIPLE first:
         *  checkFeatures report_fatal_errors on a name the target does not know. */
        bool targetHasIntDotAccum() const {     // x86 AVX512-VNNI / AVX-VNNI
            if (targetMachine == nullptr) return false;
            if (!targetMachine->getTargetTriple().isX86()) return false;
            const llvm::MCSubtargetInfo& sti = targetMachine->getMCSubtargetInfo();
            return sti.checkFeatures("+avx512vnni")
                || sti.checkFeatures("+avxvnni");
        }
        bool targetHasAvx2() const {            // x86 AVX2, the pre-VNNI tier
            if (targetMachine == nullptr) return false;
            if (!targetMachine->getTargetTriple().isX86()) return false;
            return targetMachine->getMCSubtargetInfo().checkFeatures("+avx2");
        }
        bool targetHasArmDotProd() const {      // AArch64 sdot / udot
            if (targetMachine == nullptr) return false;
            if (!targetMachine->getTargetTriple().isAArch64()) return false;
            return targetMachine->getMCSubtargetInfo().checkFeatures("+dotprod");
        }
        /** AArch64 `usdot`, the mixed unsigned x signed form. Gated separately:
         *  a target with +dotprod but no +i8mm cannot select usdot and dies. */
        bool targetHasArmI8mm() const {
            if (targetMachine == nullptr) return false;
            if (!targetMachine->getTargetTriple().isAArch64()) return false;
            return targetMachine->getMCSubtargetInfo().checkFeatures("+i8mm");
        }

        llvm::IRBuilder<>* getBuilder() {
            return this->builder;
        }

        // ── TBAA (type-based alias analysis) metadata ──────────────────────
        // Two disjoint scalar nodes, "cajeta field" and "cajeta array element",
        // both under an omnipotent-char node, so a byte access still aliases all.

        // Record `ptr`'s access kind so applyTbaaTags can tag its loads/stores.
        void recordTbaaProvenance(llvm::Value* ptr, TbaaKind kind);

        // Attach the matching !tbaa tag to every load/store whose pointer has
        // recorded provenance. Idempotent; run once per module after codegen.
        void applyTbaaTags();

        // Allocate a stack slot in the CURRENT function's ENTRY block rather than
        // at the insertion point: an alloca inside a loop re-allocates on every
        // iteration and overflows the stack at -O0. Falls back when no function.
        llvm::AllocaInst* createEntryAlloca(llvm::Type* ty, const std::string& name = "");

        CajetaTypePtr getInitializerType() const;

        void setInitializerType(CajetaTypePtr initializerType);

        const string& getSourcePath() const {
            return sourcePath;
        }

        void setSourcePath(const string& sourcePath) {
            this->sourcePath = sourcePath;
        }

        // True when this module's source was a script-shaped unit rewritten into
        // implicit-class form; the package/path agreement check is then skipped.
        void setScriptUnit(bool v) { scriptUnit = v; }
        bool isScriptUnit() const { return scriptUnit; }

        // This module is RESOLVED with no intention of generating code (`--lint`),
        // which paths with no function to emit into — notably locals — must see.
        bool isResolutionOnly() const { return resolutionOnly; }
        void setResolutionOnly(bool v) { resolutionOnly = v; }

        // The unit's top-level session-binding names: codegen promotes these owners
        // to the runtime session registry instead of the entry's drop frame.
        void setScriptBindingNames(std::vector<string> names) {
            scriptBindingNames = std::set<string>(names.begin(), names.end());
        }
        bool isScriptBindingName(const string& name) const {
            return scriptBindingNames.find(name) != scriptBindingNames.end();
        }
        const std::set<string>& getScriptBindingNames() const {
            return scriptBindingNames;
        }

        // Binding names come from top-level declarations, but a block-local can
        // SHADOW one, so this is true only while the entry's DIRECT statements run.
        void armScriptRootBlock() { scriptRootBlockPending = true; }
        bool consumeScriptRootBlockPending() {
            bool v = scriptRootBlockPending;
            scriptRootBlockPending = false;
            return v;
        }
        bool isScriptEntryTopLevel() const { return scriptEntryTopLevel; }
        bool setScriptEntryTopLevel(bool v) {
            bool prev = scriptEntryTopLevel;
            scriptEntryTopLevel = v;
            return prev;
        }

        void setSessionState(SessionState* s) { sessionState = s; }
        SessionState* getSessionState() const { return sessionState; }
        void setScriptHostName(const string& n) { scriptHostName = n; }
        const string& getScriptHostName() const { return scriptHostName; }

        // mapScriptLine turns a wrapper line into the host line (identity for an
        // ordinary module); scriptDiagFile is the name diagnostics should carry.
        void setScriptLineMap(ScriptLineMap m) { scriptLineMap = std::move(m); }
        int mapScriptLine(int wrapperLine) const {
            return cajeta::mapScriptLine(scriptLineMap, wrapperLine);
        }
        string scriptDiagFile() const {
            return scriptHostName.empty() ? sourcePath : scriptHostName;
        }
        void setScriptCurrentHostLine(int line) { scriptCurrentHostLine = line; }
        int getScriptCurrentHostLine() const { return scriptCurrentHostLine; }

        void setScriptSyntheticTail(bool v) { scriptSyntheticTail = v; }
        bool hasScriptSyntheticTail() const { return scriptSyntheticTail; }
        // Taken, not read: a marked statement's own codegen may run nested
        // expression statements, and the mark belongs to one statement.
        void setScriptResultPending(bool v) { scriptResultPending = v; }
        bool takeScriptResultPending() {
            bool v = scriptResultPending;
            scriptResultPending = false;
            return v;
        }

        // The file currently parsed INTO this module, remapped. The stdlib is many
        // files in one synthetic module, so parseStdlibInto sets it per file.
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

        bool isClasspathOrigin() const {
            return classpathOrigin;
        }

        void setClasspathOrigin(bool on) {
            classpathOrigin = on;
        }

        map<string, map<string, QualifiedNamePtr>>& getImports() {
            return imports;
        }

        // Push before walking a template body with concrete arguments; pop after.
        void pushTypeSubstitution(std::map<std::string, CajetaTypePtr> frame) {
            typeSubstitutionStack.push_back(std::move(frame));
        }
        void popTypeSubstitution() {
            if (!typeSubstitutionStack.empty()) typeSubstitutionStack.pop_back();
        }
        // The type bound to `name` by the TOP frame, or null. Only the top is
        // consulted; parameters never leak across nested instantiations.
        CajetaTypePtr lookupTypeParameter(const std::string& name) const {
            if (typeSubstitutionStack.empty()) return nullptr;
            const auto& top = typeSubstitutionStack.back();
            auto it = top.find(name);
            return it == top.end() ? nullptr : it->second;
        }
        // The top frame (null when empty) so a caller can inherit its bindings.
        const std::map<std::string, CajetaTypePtr>* getCurrentTypeSubstitution() const {
            if (typeSubstitutionStack.empty()) return nullptr;
            return &typeSubstitutionStack.back();
        }
        size_t typeSubstitutionDepth() const {
            return typeSubstitutionStack.size();
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

        // Reflection-usage accumulator, reset per compile. forcesAll ⇒ keep all;
        // `sites` are narrow contributions the Compiler resolves after quiescence.
        struct ReflSite {
            enum Kind { BoundClosure, ForNameLiteral, PackageLiteral, Annotated,
                        MethodAnnotated };
            Kind kind;
            std::string selector;  // T canonical / class name / package / anno short
        };
        struct ReflectionKeep {
            bool forcesAll = false;
            std::vector<ReflSite> sites;
            // Why each forces-ALL site does, so a lean build can suggest a selector.
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

        // Aspect registry — see the aspectClasses field. Every declaration lands
        // during parse, before any per-method codegen walks the list.
        static void registerAspectClass(CajetaClassPtr klass) {
            aspectClasses.push_back(std::move(klass));
        }
        static const vector<CajetaClassPtr>& getAspectClasses() {
            return aspectClasses;
        }

        // Pointcut-matching pass: resolve each advice method's pointcut and push an
        // AdviceMatch onto every user method it matches. Runs after all parses and
        // before any codegen.
        static void resolveAdviceMatches();

        // Component registry, filled from visitClassDeclaration per component class.
        static void registerComponent(ComponentDescriptorPtr c) {
            componentClasses.push_back(std::move(c));
        }
        static const vector<ComponentDescriptorPtr>& getComponentClasses() {
            return componentClasses;
        }

        // @Factory registry. resolveDependencyGraph walks both registries, so an
        // @Inject resolves to a component ctor or a provider (both = ambiguity).
        static void registerFactory(FactoryDescriptorPtr f) {
            factoryClasses.push_back(std::move(f));
        }
        static const vector<FactoryDescriptorPtr>& getFactoryClasses() {
            return factoryClasses;
        }

        // Active profile for component filtering; the CLI or test driver overrides.
        static const string& getActiveProfile() { return activeProfile; }
        static void setActiveProfile(const string& p) { activeProfile = p; }

        // DI graph validation: filter by profile, apply @TestComponent overrides,
        // validate each @Inject. Throws on missing, circular or ambiguous.
        static void resolveDependencyGraph();

        // Drive deferred prototypes to a fixed point: lay out every class whose
        // supertypes are all non-placeholder. Runs after parse, before Phase 1.
        static void buildPendingPrototypes();

        // Re-run per-(class, iface) vtable synthesis for classes that skipped a
        // then-placeholder interface. Idempotent; called once per codegen pass.
        void completePendingInterfaceVTables();

        // Post-parse defense in depth: report any class still flagged placeholder,
        // meaning the pre-scan vouched for a name the parse never filled in.
        static void validatePlaceholders();

        // A Function* safe to use as a callee inserted via `builder`: the original
        // when it shares the builder's llvm::Module, else a module-local extern
        // decl, since a dangling cross-module Function* fails verifyModule.
        static llvm::Function* ensureFunctionVisible(
            llvm::IRBuilder<>* builder,
            llvm::Function* original,
            llvm::FunctionType* fnType);

        // The same for CONSTANT contexts (vtable / RTTI initializers, where there
        // is no insertion point): the original, or a `targetModule`-local decl.
        static llvm::Function* ensureFunctionInModule(
            llvm::Module* targetModule,
            llvm::Function* original);
        static llvm::Constant* ensureGlobalInModule(
            llvm::Module* targetModule,
            llvm::GlobalVariable* original);

        // The module currently being walked, or null outside any walk; read by call
        // sites that could not thread a module parameter through.
        static CajetaModulePtr getActiveModule() { return activeModule; }
        static void setActiveModule(CajetaModulePtr m) { activeModule = m; }

        static CajetaModulePtr getCurrentCodegenModule() { return currentCodegenModule; }
        static void setCurrentCodegenModule(CajetaModulePtr m) { currentCodegenModule = m; }

        static CajetaModulePtr getReuseEmitModule() { return reuseEmitModule; }
        static void setReuseEmitModule(CajetaModulePtr m) { reuseEmitModule = m; }

        static CajetaModulePtr getActiveUnitModule() { return activeUnitModule; }
        static void setActiveUnitModule(CajetaModulePtr m) { activeUnitModule = m; }
        // Where a USER-TYPED specialization emits: the innermost codegen frame, else
        // the unit being compiled. Null leaves the default rule intact.
        static CajetaModulePtr sessionEmitTarget() {
            if (currentCodegenModule) return currentCodegenModule;
            return activeUnitModule;
        }

        // Per-test generation counter for the reuse path. Caches keyed on persistent
        // stdlib objects compare their stored epoch to it and self-invalidate.
        static uint64_t getReuseEpoch() { return reuseEpoch; }
        static void bumpReuseEpoch() { ++reuseEpoch; }

        static llvm::Module* getCurrentEmitLlvmModule() { return currentEmitLlvmModule; }
        static void setCurrentEmitLlvmModule(llvm::Module* m) { currentEmitLlvmModule = m; }

        // Per-module debug-loc id range from the JIT host's append-only registry,
        // so unchanged modules keep their bases. -1 = the dense global allocator.
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

        // Clear cross-Compiler module/method bookkeeping, so each fresh Compiler
        // starts with empty static state.
        static void resetGlobals();

        // Test stdlib-reuse support: captureBaseline snapshots the module-level
        // registries; restoreBaseline drops user state and re-pins the stdlib.
        static void captureBaseline();
        static void restoreBaseline();
        // A second baseline slot for "stdlib + sibling sweep", restored
        // independently of the pristine stdlib baseline on a warm request.
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

        // Share the whole-program DCE keep-set with this module.
        void setKeepSet(std::shared_ptr<const std::set<std::string>> ks) {
            keepSet = std::move(ks);
        }
        // Should class `canon` keep its reflection registration ctor? All classes
        // are kept until a keep-set is INSTALLED; after that only its members.
        bool keepsClass(const std::string& canon) const {
            if (!keepSet) return true;
            return keepSet->count(canon) > 0;
        }

        // Re-derive the embedded source-file name to a machine-independent form
        // once the flags are set, so the same source yields identical bitcode.
        void canonicalizeSourceFileName();

        // The source path in machine-independent form — what every IR-embedded
        // path uses. Compile-time DIAGNOSTICS keep the raw absolute path.
        std::string remappedSourcePath() const {
            return remapSourcePath(sourcePath, sourceRoot,
                                   compilerFlags.debugPrefixMap);
        }

        // The pure mapping behind canonicalizeSourceFileName, exposed for tests:
        // `--debug-prefix-map`, else a sourceRoot-relative '/'-normalized path.
        static std::string remapSourcePath(const std::string& sourcePath,
                                           const std::string& sourceRoot,
                                           const std::string& debugPrefixMap);

        // Incremental compilation (Phase 2) — obligation capture/serialize.
        const std::set<std::string>& getInstantiationObligations() const {
            return instantiationObligations;
        }
        // Record `inst` as an obligation of `triggering` iff it is a genuine
        // cross-module instantiation. No-op when same-module or either is null.
        static void noteCrossModuleInstantiation(
            const CajetaModulePtr& triggering, const CajetaClassPtr& inst);
        // The method-template twin: the instantiation lands in the HOST class's
        // module, so replaying the class obligation does not re-create it.
        static void noteCrossModuleMethodInstantiation(
            const CajetaModulePtr& triggering, const MethodPtr& inst);
        // The static-field twin: a foldable static's global is defined only where
        // some module references it, so a skipped module would drop the symbol.
        static void noteCrossModuleStaticFieldRef(
            const CajetaModulePtr& triggering, const CajetaClassPtr& owner,
            const std::string& fieldName);
        // Write the obligations, one sorted canonical name per line, to a sidecar
        // beside the emitted IR. No-op when empty; a stale sidecar is removed.
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
        // Write the obligations to the manifest slot — ALWAYS, explicitly empty
        // when the set is: slot absence means "never built". False on I/O failure.
        bool writeObligationsToSlot() const;
        // Serialize the module's current llvm::Module to the .bc slot
        // (atomic temp+rename). Returns false on I/O failure.
        bool writeBitcodeToSlot() const;
        // Replace this module's llvm::Module with the bitcode at the .bc slot, in
        // the SAME LLVMContext. False (untouched) leaves the caller to go dirty.
        bool loadBitcodeFromSlot();

        // Intern a source path as a module-global `const char*` for debug-mode
        // source tagging; the same path returns the same Constant.
        llvm::Constant* getOrCreateSourceFileConstant(const std::string& path);

        void pushLoopContext(llvm::BasicBlock* cont, llvm::BasicBlock* brk,
                             size_t dropFrameDepth = 0) {
            loopContextStack.push_back(
                {cont, brk, pendingLoopLabel, tryFinallyStack.size(),
                 dropFrameDepth});
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

        // Push one entry per enclosing try body: its finally, or null for a
        // catch-only try. shared_ptr<void> avoids a Statement.h include here.
        void pushTryFinally(std::shared_ptr<void> finallyStmt) {
            tryFinallyStack.push_back(std::move(finallyStmt));
        }
        void popTryFinally() {
            if (!tryFinallyStack.empty()) tryFinallyStack.pop_back();
        }
        std::vector<std::shared_ptr<void>>& getTryFinallyStack() {
            return tryFinallyStack;
        }
        // Detach the stack for a nested body's codegen: a lambda is generated
        // inline, and its enclosing method's open try frames must not bleed in.
        std::vector<std::shared_ptr<void>> takeTryFinally() {
            std::vector<std::shared_ptr<void>> saved = std::move(tryFinallyStack);
            tryFinallyStack.clear();
            return saved;
        }
        void restoreTryFinally(std::vector<std::shared_ptr<void>> saved) {
            tryFinallyStack = std::move(saved);
        }

        // Full per-function detach: tryFinally + tryCatch + loops + pending label. A
        // method body can be generated NESTED in another's codegen, and a `return`
        // there must not emit the CALLER's try-frame unwind.
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

        // Parse the embedded cajeta_runtime bitcode and merge it into this module.
        // Idempotent; true on success.
        bool linkRuntime();

        // Look up a runtime helper by name (linkRuntime() first); null if missing.
        // `explicitTarget` names the llvm::Module the returned decl must be
        // co-resident with when that differs from emitTargetLlvmModule().
        llvm::Function* getRuntimeFunction(const std::string& name,
                                           llvm::Module* explicitTarget = nullptr);

        // The llvm::Module that IR created right now should land in: the module of
        // the function the builder is inserting into, falling back to this module's
        // own when there is no insert point.
        llvm::Module* emitTargetLlvmModule();

        void writeIRFileTarget() {
            string targetPath = archiveRoot + archivePath;
            std::error_code ec;

            string targetDirs = targetPath.substr(0, targetPath.rfind("/"));
            std::filesystem::create_directories(targetDirs);
            llvm::raw_fd_ostream out(targetPath, ec, llvm::sys::fs::CD_CreateAlways);
            llvmModule->print(out, nullptr);
        }

        void setCurrentMethod(MethodPtr method) {
            this->currentMethod = method;
        }

        // True while emitting a lambda body whose synthesized function returns a
        // class POINTER: a lambda has no Method for the return-flag protocol.
        void setLambdaClassPtrReturn(bool v) { lambdaClassPtrReturn = v; }
        bool isLambdaClassPtrReturn() const { return lambdaClassPtrReturn; }

        MethodPtr getCurrentMethod() {
            return currentMethod;
        }

        list<MethodPtr> getAllMethods() {
            list<MethodPtr> result;
            for (auto entry:  structures) {
                // A template's body references unresolved parameters; instantiations
                // are codegen'd through their own entries in `structures`.
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

