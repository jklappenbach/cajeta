//
// Created by James Klappenbach on 10/22/22.
//

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include "llvm/TargetParser/Triple.h"
#include "../error/Exception.h"
#include "../error/DiagnosticEngine.h"
#include "../xref/XrefIndex.h"

#include "CajetaModule.h"
#include "../logging/CajetaLogger.h"
#include "Compiler.h"
#include "../method/Method.h"
#include "../method/ComponentInjectMethod.h"
#include "../method/FactoryProviderMethod.h"
#include "../type/StructureMetadata.h"
#include "../type/CajetaClass.h"
#include "../runtime/EmbeddedRuntime.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"

namespace cajeta {
    // thread-safe-compiler Unit 3: per-compile registries thread_local.
    thread_local map<string, MethodPtr> CajetaModule::methods;
    thread_local map<string, CajetaModulePtr> CajetaModule::strutureToModule;
    thread_local CajetaModulePtr CajetaModule::activeModule;
    thread_local CajetaModulePtr CajetaModule::currentCodegenModule;
    // U5 concurrency-first: each thread primes its OWN stdlib (memory-heavy,
    // temporary — the frozen-shared optimization replaces this).
    thread_local CajetaModulePtr CajetaModule::reuseEmitModule;
    thread_local llvm::Module* CajetaModule::currentEmitLlvmModule = nullptr;
    thread_local uint64_t CajetaModule::reuseEpoch = 0;
    thread_local CajetaModulePtr CajetaModule::stdlibModule;
    thread_local std::function<void(const std::string&)> CajetaModule::stdlibImportHook;
    thread_local std::function<bool(const std::string&)> CajetaModule::userMaterializeHook;
    thread_local map<string, CajetaModulePtr> CajetaModule::moduleVariables;
    thread_local vector<CajetaClassPtr> CajetaModule::aspectClasses;
    thread_local vector<CajetaModule::ComponentDescriptorPtr> CajetaModule::componentClasses;
    thread_local vector<CajetaModule::FactoryDescriptorPtr> CajetaModule::factoryClasses;
    thread_local string CajetaModule::activeProfile = "prod";

    CajetaModule::CajetaModule(llvm::LLVMContext* llvmContext,
        QualifiedNamePtr qName,
        string targetTriple,
        llvm::TargetMachine* targetMachine) {
        this->llvmContext = llvmContext;
        this->targetTriple = targetTriple;
        this->targetMachine = targetMachine;
        this->qName = qName;
        this->archivePath = qName->toCanonical() + CAJETA_IR_EXTENSION;
        llvmModule = new llvm::Module(qName->toCanonical(), *llvmContext);
        llvmModule->setSourceFileName(qName->toCanonical());
        llvmModule->setDataLayout(targetMachine->createDataLayout());
        // LLVM 21 narrowed Module::setTargetTriple to take llvm::Triple;
        // 18 / 20 take a StringRef (string). The Triple ctor accepts the
        // triple string in both, so we branch at preprocess time.
#if LLVM_VERSION_MAJOR >= 21
        llvmModule->setTargetTriple(llvm::Triple(targetTriple));
#else
        llvmModule->setTargetTriple(targetTriple);
#endif
    }

    CajetaModule::CajetaModule(llvm::LLVMContext* llvmContext,
        string sourcePath,
        string sourceRoot,
        string archiveRoot,
        string targetTriple,
        llvm::TargetMachine* targetMachine) {
        this->llvmContext = llvmContext;
        this->sourcePath = sourcePath;
        this->sourceRoot = sourceRoot;
        this->archiveRoot = archiveRoot;
        this->targetTriple = targetTriple;
        this->targetMachine = targetMachine;

        int suffixIndex = sourcePath.find(CAJETA_EXTENSION);
        if (suffixIndex >= 0) {
            // `temp` is sourcePath stripped of sourceRoot prefix and ".cajeta"
            // suffix. Whether it starts with PATH_SEPARATOR depends on whether
            // the caller's sourceRoot ended with one. CLI driver (Compiler::compile
            // with positional args) appends a trailing '/'; direct createModule
            // callers (tests) typically don't. Normalize the leading separator
            // out so the package-extraction math is independent of that detail.
            string temp = sourcePath.substr(sourceRoot.size(), suffixIndex - sourceRoot.size());
#ifdef _WIN32
            // The filesystem hands back Windows paths with '\' separators, but
            // package derivation below keys off PATH_SEPARATOR ('/'). Normalize
            // so `<root>\test\D.cajeta` yields package "test", not "". Backslash
            // is never a valid filename char on Windows, so this is lossless.
            std::replace(temp.begin(), temp.end(), '\\', '/');
#endif
            if (!temp.empty() && temp[0] == PATH_SEPARATOR) {
                temp.erase(0, 1);
            }
            int lastSep = (int) temp.rfind(PATH_SEPARATOR);
            string moduleName = (lastSep < 0)
                ? temp
                : temp.substr(lastSep + 1);
            string packageName = (lastSep < 0)
                ? string()
                : temp.substr(0, lastSep);
            archivePath = string(1, PATH_SEPARATOR) + temp + CAJETA_IR_EXTENSION;
            replace(packageName.begin(), packageName.end(), PATH_SEPARATOR, PACKAGE_SEPARATOR);
            qName = QualifiedName::getOrInsert(moduleName, packageName);

            llvmModule = new llvm::Module(qName->toCanonical(), *llvmContext);
            llvmModule->setSourceFileName(sourcePath);
            llvmModule->setDataLayout(targetMachine->createDataLayout());
            // LLVM 21 narrowed Module::setTargetTriple to take llvm::Triple;
        // 18 / 20 take a StringRef (string). The Triple ctor accepts the
        // triple string in both, so we branch at preprocess time.
#if LLVM_VERSION_MAJOR >= 21
        llvmModule->setTargetTriple(llvm::Triple(targetTriple));
#else
        llvmModule->setTargetTriple(targetTriple);
#endif
        } else {
            cerr << "Error: Module srcPath must reference a code pModule, a file with the correct naming convention";
        }
    }

    std::string CajetaModule::remapSourcePath(const std::string& sourcePath,
                                              const std::string& sourceRoot,
                                              const std::string& debugPrefixMap) {
        auto normalize = [](std::string s) {
            std::replace(s.begin(), s.end(), '\\', '/');
            return s;
        };
        std::string path = normalize(sourcePath);

        // Honor an explicit --debug-prefix-map=<from>=<to>. Split on the
        // first '=' (the GCC/Clang convention: <from> is everything before
        // it). When the source path starts with <from>, swap that prefix.
        if (!debugPrefixMap.empty()) {
            auto eq = debugPrefixMap.find('=');
            if (eq != std::string::npos) {
                std::string from = normalize(debugPrefixMap.substr(0, eq));
                std::string to = debugPrefixMap.substr(eq + 1);
                if (!from.empty() && path.compare(0, from.size(), from) == 0) {
                    return to + path.substr(from.size());
                }
            }
        }

        // No map (or no prefix match): fall back to a sourceRoot-relative
        // path so the embedded name is still machine-independent when the
        // compiler is driven directly (tests, manual invocation) without a
        // map from the build tool.
        std::string root = normalize(sourceRoot);
        if (!root.empty() && path.compare(0, root.size(), root) == 0) {
            std::string rel = path.substr(root.size());
            if (!rel.empty() && rel[0] == '/') {
                rel.erase(0, 1);
            }
            return rel;
        }
        return path;
    }

    void CajetaModule::canonicalizeSourceFileName() {
        if (sourcePath.empty()) {
            // Synthetic modules already embed the canonical name; nothing
            // machine-specific to scrub.
            return;
        }
        llvmModule->setSourceFileName(
            remapSourcePath(sourcePath, sourceRoot, compilerFlags.debugPrefixMap));
    }

    void CajetaModule::noteCrossModuleInstantiation(
        const CajetaModulePtr& triggering, const CajetaClassPtr& inst) {
        if (!triggering || !inst) {
            return;
        }
        // Same-module instantiations travel with the module's own IR — only
        // cross-module ones (the template is owned elsewhere, e.g. stdlib)
        // can vanish when this module's codegen is skipped.
        if (inst->getModule() == triggering) {
            return;
        }
        triggering->instantiationObligations.insert(inst->toCanonical());
    }

    void CajetaModule::noteCrossModuleMethodInstantiation(
        const CajetaModulePtr& triggering, const MethodPtr& inst) {
        if (!triggering || !inst) {
            return;
        }
        // The instantiated method's codegen lands in its own module (the host
        // class's module — the template's declaring module, e.g. stdlib). Only
        // a cross-module landing can vanish when `triggering`'s codegen is
        // skipped; a same-module instantiation travels with this module's IR.
        if (inst->getModule() == triggering) {
            return;
        }
        // getMapKey(false) embeds the host-class canonical, method name,
        // value-param signature, and method-type-arg suffix — unique,
        // deterministic, and reconcilable at replay. The `::` separator marks
        // it as a method (vs. a `<…>`-only class) obligation.
        triggering->instantiationObligations.insert(inst->getMapKey(false));
    }

    void CajetaModule::noteCrossModuleStaticFieldRef(
        const CajetaModulePtr& triggering, const CajetaClassPtr& owner,
        const std::string& fieldName) {
        if (!triggering || !owner || fieldName.empty()) {
            return;
        }
        // A same-module reference travels with the module's own IR; only a
        // cross-module demand can vanish when `triggering` is skipped.
        if (owner->getModule() == triggering) {
            return;
        }
        triggering->instantiationObligations.insert(
            owner->toCanonical() + "::" + fieldName);
    }

    void CajetaModule::writeObligationsSidecar() const {
        // Path mirrors the emitted IR: swap the .ll suffix for .obligations.
        std::string path = archiveRoot + archivePath;
        const std::string irExt = CAJETA_IR_EXTENSION; // ".ll"
        if (path.size() >= irExt.size() &&
            path.compare(path.size() - irExt.size(), irExt.size(), irExt) == 0) {
            path.replace(path.size() - irExt.size(), irExt.size(), ".obligations");
        } else {
            path += ".obligations";
        }
        std::error_code ec;
        if (instantiationObligations.empty()) {
            std::filesystem::remove(path, ec); // drop any stale sidecar
            return;
        }
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        for (const auto& name : instantiationObligations) { // std::set → sorted
            out << name << "\n";
        }
    }

    bool CajetaModule::writeObligationsToSlot() const {
        if (cacheObligationsSlot.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(cacheObligationsSlot).parent_path(), ec);
        std::ofstream out(cacheObligationsSlot, std::ios::trunc);
        if (!out) return false;
        // Unlike the archive-root sidecar, an empty set still writes the
        // (empty) file: slot absence must mean "never built".
        for (const auto& name : instantiationObligations) {
            out << name << "\n";
        }
        return static_cast<bool>(out);
    }

    bool CajetaModule::writeBitcodeToSlot() const {
        if (cacheBcSlot.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(cacheBcSlot).parent_path(), ec);
        // Atomic: temp in the same dir, then rename over the slot.
        std::string tmp = cacheBcSlot + ".tmp";
        {
            std::error_code fec;
            llvm::raw_fd_ostream os(tmp, fec);
            if (fec) return false;
            llvm::WriteBitcodeToFile(*llvmModule, os);
            os.close();
            if (os.has_error()) return false;
        }
        std::filesystem::rename(tmp, cacheBcSlot, ec);
        return !ec;
    }

    bool CajetaModule::loadBitcodeFromSlot() {
        if (cacheBcSlot.empty()) return false;
        auto buf = llvm::MemoryBuffer::getFile(cacheBcSlot);
        if (!buf) return false;
        auto parsed = llvm::parseBitcodeFile((*buf)->getMemBufferRef(),
                                             *llvmContext);
        if (!parsed) {
            cerr << "cajeta: [incremental] failed to parse cached bitcode "
                 << cacheBcSlot << ": "
                 << llvm::toString(parsed.takeError()) << std::endl;
            return false;
        }
        delete llvmModule;
        llvmModule = parsed->release();
        // Every cached llvm handle below pointed INTO the module just deleted.
        // The swap runs POST-codegen (Compiler.cpp, "Dirty modules snapshot
        // post-codegen"), so they are populated by now, and codegen does still
        // reach a swapped module afterwards (a dirty module's replay/lowering
        // can instantiate into it). Drop them so the next use rebuilds against
        // the NEW module instead of handing out a freed pointer.
        // NOT cleared: the tbaa MDNode* members — metadata is owned by the
        // LLVMContext, which outlives the swap.
        sourceFileConstants.clear();
        tbaaProvenance.clear();
        return true;
    }

    llvm::IRBuilder<>* CajetaModule::getBuilder() const {
        return builder;
    }

    llvm::AllocaInst* CajetaModule::createEntryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::BasicBlock* insertBB = builder->GetInsertBlock();
        llvm::Function* fn = insertBB ? insertBB->getParent() : nullptr;
        if (fn) {
            // Insert at the top of the entry block (before any terminator),
            // so the alloca executes once on function entry regardless of how
            // deep in a loop the declaration is. Other entry allocas already
            // there are fine to precede — alloca order is immaterial.
            llvm::BasicBlock& entry = fn->getEntryBlock();
            llvm::IRBuilder<> eb(&entry, entry.begin());
            return eb.CreateAlloca(ty, nullptr, name);
        }
        return builder->CreateAlloca(ty, nullptr, name);
    }

    void CajetaModule::onPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) {
        std::vector<CajetaParser::IdentifierContext*> identifiers = ctx->qualifiedName()->identifier();
        auto itr = identifiers.begin();
        string packageName = (*itr)->getText();
        itr++;
        while (itr != identifiers.end()) {
            packageName.append(".");
            packageName.append((*itr)->getText());
            itr++;
        }

        if (qName->getPackageName() != packageName) {
            // Under lint (an active DiagnosticEngine), the file's disk path is not
            // authoritative — it may be a staged, unsaved buffer whose temp path
            // can't match its declared package. Skip the check rather than leak a
            // false-positive plain-text error into the NDJSON stream. (Full
            // compile keeps validating; routing it through the engine is part of
            // the full-compile collect-and-continue sub-phase.)
            if (DiagnosticEngine::active()) return;
            string message = "Declared package name " + packageName + " must match the compilation unit path of " +
                qName->getPackageName();
            CajetaLogger::log(ERROR, ctx, "CAJETA_ERROR_PACKAGE_MISMATCH", sourcePath, message);
        }
    }

    bool verifyImport(QualifiedNamePtr qName) {
        return true;
    }

    void CajetaModule::processMetadata(CajetaClassPtr structure) {
        structureMetadata->populate(structure);
    }

    void CajetaModule::onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) {
        auto qName = QualifiedName::fromContext(ctx->qualifiedName());
        imports[qName->getTypeName()][qName->getPackageName()] = qName;
        // xref (ide-symbol-index): record the imported type as a reference at
        // its name token, so Ctrl-click on `import a.b.Gzip;` navigates to the
        // type. Imports resolve here, not through CajetaType::fromContext, so
        // without this they carry no edge. Gated on --emit-xref; a wildcard
        // (`a.b.*`) or an unresolved name records nothing (prune drops the
        // rest). Positioned at the LEAF identifier — the type name a developer
        // Ctrl-clicks.
        if (xref::captureEnabled() && !qName->getTypeName().empty()
                && qName->getTypeName() != "*" && ctx->qualifiedName()) {
            const auto& ids = ctx->qualifiedName()->identifier();
            if (!ids.empty()) {
                if (auto* tok = ids.back()->getStart()) {
                    if (tok->getInputStream()) {
                        if (const std::string* file = xref::internSourceFile(
                                tok->getInputStream()->getSourceName())) {
                            xref::noteTypeReference(qName->toCanonical(), *file,
                                (int) tok->getLine(),
                                (int) tok->getCharPositionInLine());
                        }
                    }
                }
            }
        }
        // Lazy stdlib: an import of an on-demand package (e.g. cajeta.math)
        // triggers that package's prescan + enqueue so its types resolve
        // during this parse and are fully parsed at the next drain point.
        if (stdlibImportHook) {
            stdlibImportHook(qName->getPackageName());
        }
    }

    void CajetaModule::onStructureDeclaration(std::any any) {
        // Type-declaration children that don't yield a CajetaClass (e.g. enums,
        // which register their constants in a side-table instead) return a
        // null `any`; skip those rather than throw bad_any_cast.
        if (!any.has_value()) return;
        try {
            CajetaClassPtr structure = std::any_cast<CajetaClassPtr>(any);
            if (structure) {
                structures[structure->toCanonical()] = structure;
            }
        } catch (const std::bad_any_cast&) {
            // Not a CajetaClass — caller didn't return one (e.g. enum
            // declaration). Nothing to register on the module here.
        }
    }

    void CajetaModule::completePendingInterfaceVTables() {
        for (auto& [canon, structure] : structures) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(structure);
            if (klass && klass->hasPendingIfaceVTables()
                    && !klass->isPlaceholder()) {
                klass->resolveImplementedInterfaces();
                klass->synthesizeInterfaceVTables();
            }
        }
    }

    CajetaTypePtr CajetaModule::getInitializerType() const {
        return initializerType;
    }

    void CajetaModule::setInitializerType(CajetaTypePtr initializerType) {
        this->initializerType = initializerType;
    }

    CajetaModulePtr CajetaModule::create(
        llvm::LLVMContext* llvmContext,
        string sourcePath,
        string sourceRoot,
        string archiveRoot,
        string targetTriple,
        llvm::TargetMachine* targetMachine) {
        CajetaModulePtr result = make_shared<CajetaModule>(llvmContext, sourcePath, sourceRoot, archiveRoot, targetTriple, targetMachine);
        result->structureMetadata = make_shared<StructureMetadata>(result);
        strutureToModule[result->qName->toCanonical()] = result;
        return result;
    }

    void CajetaModule::resetGlobals() {
        strutureToModule.clear();
        moduleVariables.clear();
        methods.clear();
        aspectClasses.clear();
        componentClasses.clear();
        factoryClasses.clear();
        activeProfile = "prod";
        Method::getArchive().clear();
        stdlibModule.reset();
        activeModule.reset();
        currentCodegenModule.reset();
    }

    namespace {
        // Snapshot of the module-level global registries, taken once after the
        // pristine stdlib build and reassigned before each reusing test. Holds
        // the stdlib singleton so restore re-pins it (the persistent module),
        // and the method archive so cross-file forward refs reset too.
        struct ModuleGlobalsBaseline {
            bool valid = false;
            map<string, MethodPtr> methods;
            map<string, CajetaModulePtr> strutureToModule;
            map<string, CajetaModulePtr> moduleVariables;
            vector<CajetaClassPtr> aspectClasses;
            vector<CajetaModule::ComponentDescriptorPtr> componentClasses;
            vector<CajetaModule::FactoryDescriptorPtr> factoryClasses;
            string activeProfile;
            map<string, MethodPtr> methodArchive;
            CajetaModulePtr stdlibModule;
        };
        ModuleGlobalsBaseline g_moduleBaseline;
        // lint-server sibling context (spec §4): a SECOND slot holding the
        // module registries after the sibling sweep, restored independently of
        // the pristine g_moduleBaseline on a warm request. See CajetaType's
        // matching context baseline.
        ModuleGlobalsBaseline g_moduleContextBaseline;

        void captureModuleBaselineInto(
                ModuleGlobalsBaseline& b,
                const map<string, MethodPtr>& methods,
                const map<string, CajetaModulePtr>& strutureToModule,
                const map<string, CajetaModulePtr>& moduleVariables,
                const vector<CajetaClassPtr>& aspectClasses,
                const vector<CajetaModule::ComponentDescriptorPtr>& componentClasses,
                const vector<CajetaModule::FactoryDescriptorPtr>& factoryClasses,
                const string& activeProfile,
                const CajetaModulePtr& stdlibModule) {
            b.methods = methods;
            b.strutureToModule = strutureToModule;
            b.moduleVariables = moduleVariables;
            b.aspectClasses = aspectClasses;
            b.componentClasses = componentClasses;
            b.factoryClasses = factoryClasses;
            b.activeProfile = activeProfile;
            b.methodArchive = Method::getArchive();
            b.stdlibModule = stdlibModule;
            b.valid = true;
        }
    }

    void CajetaModule::captureBaseline() {
        captureModuleBaselineInto(g_moduleBaseline, methods, strutureToModule,
            moduleVariables, aspectClasses, componentClasses, factoryClasses,
            activeProfile, stdlibModule);
    }

    void CajetaModule::restoreBaseline() {
        if (!g_moduleBaseline.valid) return;
        methods = g_moduleBaseline.methods;
        strutureToModule = g_moduleBaseline.strutureToModule;
        moduleVariables = g_moduleBaseline.moduleVariables;
        aspectClasses = g_moduleBaseline.aspectClasses;
        componentClasses = g_moduleBaseline.componentClasses;
        factoryClasses = g_moduleBaseline.factoryClasses;
        activeProfile = g_moduleBaseline.activeProfile;
        Method::getArchive() = g_moduleBaseline.methodArchive;
        stdlibModule = g_moduleBaseline.stdlibModule;
        // Transient per-compile pointers must not leak across tests.
        activeModule.reset();
        currentCodegenModule.reset();
    }

    void CajetaModule::captureContextBaseline() {
        captureModuleBaselineInto(g_moduleContextBaseline, methods,
            strutureToModule, moduleVariables, aspectClasses, componentClasses,
            factoryClasses, activeProfile, stdlibModule);
    }

    void CajetaModule::restoreContextBaseline() {
        if (!g_moduleContextBaseline.valid) return;
        methods = g_moduleContextBaseline.methods;
        strutureToModule = g_moduleContextBaseline.strutureToModule;
        moduleVariables = g_moduleContextBaseline.moduleVariables;
        aspectClasses = g_moduleContextBaseline.aspectClasses;
        componentClasses = g_moduleContextBaseline.componentClasses;
        factoryClasses = g_moduleContextBaseline.factoryClasses;
        activeProfile = g_moduleContextBaseline.activeProfile;
        Method::getArchive() = g_moduleContextBaseline.methodArchive;
        stdlibModule = g_moduleContextBaseline.stdlibModule;
        activeModule.reset();
        currentCodegenModule.reset();
    }

    void CajetaModule::invalidateContextBaseline() {
        g_moduleContextBaseline = ModuleGlobalsBaseline{};
    }

    // Walks the registered aspects, identifies each advice method by
    // its @Before/@After/@Around/@AfterReturning/@AfterThrowing
    // annotation, resolves the pointcut's `.class` argument, and
    // pushes an AdviceMatch onto every user method that satisfies the
    // pointcut. See AspectModel.md § Implementation roadmap A3.
    //
    // Pointcut shape discriminator (v1): if the pointcut class name
    // resolves to a registered class in the global structure map,
    // it's type-based ("every method on this class"). Otherwise it's
    // assumed to be marker-annotation mode ("every method annotated
    // with this name"). `annotation` types register in canonicalMap (so
    // they resolve as type tokens, e.g. classesAnnotated<@A>()) but NOT in
    // the structure map, so absence-from-structures is still a reliable proxy.
    void CajetaModule::resolveAdviceMatches() {
        if (aspectClasses.empty()) return;

        auto classifyAdvice =
            [](const MethodPtr& m, AdviceKind& outKind, string& outName) -> bool {
                static const std::pair<const char*, AdviceKind> kinds[] = {
                    { "Before",         AdviceKind::Before },
                    { "After",          AdviceKind::After },
                    { "Around",         AdviceKind::Around },
                    { "AfterReturning", AdviceKind::AfterReturning },
                    { "AfterThrowing",  AdviceKind::AfterThrowing },
                };
                for (auto& [name, kind] : kinds) {
                    if (m->findAnnotation(name)) {
                        outKind = kind;
                        outName = name;
                        return true;
                    }
                }
                return false;
            };

        // Build a set of aspect classes for "is this method's owner
        // an aspect?" checks during the user-method walk. Advice
        // doesn't apply to other aspects' methods; users keep that
        // separate (cross-aspect coordination is via DI, not AoP).
        set<CajetaClassPtr> aspectSet(aspectClasses.begin(), aspectClasses.end());

        // Resolve a short pointcut name (e.g. "Audited") to a
        // registered class, if one exists. Walks strutureToModule
        // and matches on short typeName — pointcuts in v1 are
        // written without package qualifiers (`@Before(Audited.class)`),
        // and short-name lookup matches how the rest of the
        // compiler resolves user-written type names.
        auto resolveClassByShortName =
            [](const string& shortName) -> CajetaClassPtr {
                for (auto& [canonical, mod] : strutureToModule) {
                    if (!mod) continue;
                    auto& structs = mod->getStructures();
                    auto it = structs.find(canonical);
                    if (it == structs.end() || !it->second) continue;
                    if (it->second->isAnnotation()) continue;  // marker, not a type pointcut
                    auto qn = it->second->getQName();
                    if (qn && qn->getTypeName() == shortName) {
                        return it->second;
                    }
                }
                return nullptr;
            };

        for (auto& aspect : aspectClasses) {
            if (!aspect) continue;
            for (auto& [methodKey, adviceMethod] : aspect->getMethods()) {
                if (!adviceMethod) continue;
                AdviceKind kind = AdviceKind::Before;
                string adviceAnnotName;
                if (!classifyAdvice(adviceMethod, kind, adviceAnnotName)) {
                    continue;
                }
                auto ann = adviceMethod->findAnnotation(adviceAnnotName);
                if (!ann) continue;  // shouldn't happen — classifyAdvice just found it
                string pointcutClassName = ann->getClassRef();
                if (pointcutClassName.empty()) {
                    // Pointcut argument wasn't a class literal — v1
                    // doesn't recognize any other form, so this
                    // advice silently doesn't match anything.
                    // Higher-quality diagnostics ship in a later
                    // pass (deferred to A12 composition tests).
                    continue;
                }

                CajetaClassPtr pointcutClass = resolveClassByShortName(pointcutClassName);
                PointcutShape shape = pointcutClass
                    ? PointcutShape::Type
                    : PointcutShape::MarkerAnnotation;

                // Walk every user class once via strutureToModule
                // (the process-wide canonical->module map). For each
                // class, walk its declared methods. Skip aspect-
                // owned methods so advice doesn't fire on advice
                // bodies.
                for (auto& [canonical, mod] : strutureToModule) {
                    if (!mod) continue;
                    auto& structs = mod->getStructures();
                    auto it = structs.find(canonical);
                    if (it == structs.end() || !it->second) continue;
                    auto userClass = it->second;
                    if (aspectSet.count(userClass)) continue;

                    for (auto& [umkey, userMethod] : userClass->getMethods()) {
                        if (!userMethod) continue;
                        bool matched = false;
                        if (shape == PointcutShape::MarkerAnnotation) {
                            if (userMethod->findAnnotation(pointcutClassName)) {
                                matched = true;
                            }
                        } else {
                            // Type-based pointcut (A12): the method
                            // belongs to the pointcut class OR to a
                            // descendant of it. The pointcut declares
                            // "every method on T or T's subtypes" —
                            // walking the superClasses + implemented
                            // interfaces lists from the user's class
                            // catches the inheritance / implementation
                            // chain.
                            if (userClass == pointcutClass) {
                                matched = true;
                            } else {
                                // BFS over ancestors: superclass chain
                                // plus implemented interfaces. The
                                // implementedInterfaces list is
                                // resolved at generatePrototype time
                                // (CajetaClass::resolveImplementedInterfaces)
                                // so by codegen-pointcut time the
                                // pointers are stable.
                                std::vector<CajetaClassPtr> queue;
                                queue.push_back(userClass);
                                std::set<CajetaClassPtr> seen;
                                while (!queue.empty()) {
                                    auto cur = queue.back();
                                    queue.pop_back();
                                    if (!cur || !seen.insert(cur).second) continue;
                                    if (cur == pointcutClass) {
                                        matched = true;
                                        break;
                                    }
                                    for (auto& sup : cur->getSuperClasses()) {
                                        if (sup) queue.push_back(sup);
                                    }
                                    for (auto& iface : cur->getImplementedInterfaces()) {
                                        if (iface) queue.push_back(iface);
                                    }
                                }
                            }
                        }
                        if (matched) {
                            AdviceMatch am;
                            am.aspectClass = aspect;
                            am.adviceMethod = adviceMethod;
                            am.kind = kind;
                            am.shape = shape;
                            userMethod->addAdviceMatch(std::move(am));
                        }
                    }
                }
            }
        }

        // A7: stable-sort each user method's matchingAdvice by the
        // @Order(n) annotation on the advice method. Advice without
        // @Order falls through with INT64_MAX (placed after
        // explicitly-ordered advice; relative declaration order
        // preserved among themselves thanks to stable_sort).
        //
        // This ordering propagates automatically through A4-A6: the
        // emit helpers iterate matchingAdvice in vector order, so
        // @Before/@After/@AfterReturning/@AfterThrowing all fire in
        // the chosen sequence. emitAroundWrapper walks the same
        // sorted list to build the @Around chain (A7's other half).
        auto readOrder = [](const AdviceMatch& m) -> int64_t {
            if (!m.adviceMethod) return INT64_MAX;
            auto ann = m.adviceMethod->findAnnotation("Order");
            if (!ann) return INT64_MAX;
            // @Order(n) — unnamed integer arg. getInt routes through
            // findArg("value") which matches the unnamed-arg form.
            auto* arg = ann->findArg("value");
            if (!arg || arg->kind != AnnotationArgKind::Int64) {
                return INT64_MAX;
            }
            return arg->i64Val;
        };
        for (auto& [canonical, mod] : strutureToModule) {
            if (!mod) continue;
            for (auto& [structName, klass] : mod->getStructures()) {
                if (!klass) continue;
                for (auto& [mkey, method] : klass->getMethods()) {
                    if (!method) continue;
                    auto& matches = method->getMutableMatchingAdvice();
                    if (matches.size() < 2) continue;
                    std::stable_sort(matches.begin(), matches.end(),
                        [&](const AdviceMatch& a, const AdviceMatch& b) {
                            return readOrder(a) < readOrder(b);
                        });
                }
            }
        }
    }

    // Walks the registered components, filters by active profile,
    // applies @TestComponent overrides, then validates the DI graph
    // (missing implementation, circular dependency, ambiguous
    // resolution). See AspectModel.md § A8.
    //
    // Field-level @Inject is the only injection shape inspected here;
    // constructor-parameter @Inject lands alongside A9's codegen
    // pass, which is where the new-call lowering needs the resolved
    // dependency anyway.
    //
    // No codegen is emitted by this pass — A9 reads the validated
    // graph state to synthesize get_X / make_X helpers.
    void CajetaModule::resolveDependencyGraph() {
        if (componentClasses.empty()) return;

        // Filter by profile. A component with no @Profile is
        // profile-neutral (always included). A component with one or
        // more @Profile annotations is included only if at least
        // one matches the active profile.
        auto profileMatches = [](const ComponentDescriptorPtr& c) {
            if (c->profiles.empty()) return true;
            for (auto& p : c->profiles) {
                if (p == activeProfile) return true;
            }
            return false;
        };

        // Active set after profile filtering. Two passes follow:
        // (1) collect @TestComponent overrides keyed by target type;
        // (2) walk the registry, dropping @TestComponent entries
        //     when not in test mode, dropping @Component entries
        //     whose type is overridden in test mode.
        const bool testMode = (activeProfile == "test");

        // Type → descriptor map built from the filtered + override-
        // applied set. Used by the @Inject resolver below.
        vector<ComponentDescriptorPtr> active;
        // Test-mode override: an active @TestComponent masks every
        // non-test @Component that implements an interface the double
        // also implements, so the double stands in at that interface's
        // injection sites. Keyed on interface canonical names. A double
        // with no interface masks nothing — it is injectable only by its
        // own concrete type (preserves the own-type stub behavior).
        set<string> testOverriddenInterfaces;
        if (testMode) {
            for (auto& c : componentClasses) {
                if (!c || !c->klass) continue;
                if (!c->isTestComponent) continue;
                if (!profileMatches(c)) continue;
                for (auto& iface : c->klass->getImplementedInterfaces()) {
                    if (iface && iface->getQName()) {
                        testOverriddenInterfaces.insert(iface->getQName()->toCanonical());
                    }
                }
            }
        }
        for (auto& c : componentClasses) {
            if (!c || !c->klass) continue;
            if (!profileMatches(c)) continue;
            if (c->isTestComponent && !testMode) continue;
            // In test mode, drop a non-test @Component that shares an
            // interface with an active @TestComponent.
            if (testMode && !c->isTestComponent) {
                bool masked = false;
                for (auto& iface : c->klass->getImplementedInterfaces()) {
                    if (iface && iface->getQName()
                            && testOverriddenInterfaces.count(
                                   iface->getQName()->toCanonical())) {
                        masked = true;
                        break;
                    }
                }
                if (masked) continue;
            }
            active.push_back(c);
        }

        // Type-keyed lookup tables. v1 keys by both the canonical
        // qualified name AND the short type name so consumers can
        // write `@Inject Database db;` without writing the package.
        // Inheritance / interface implementation walks (the doc's
        // "implementer-of-Persister" case) ship with A12 once the
        // ancestor-resolution path is stable.
        map<string, vector<ComponentDescriptorPtr>> byCanonical;
        map<string, vector<ComponentDescriptorPtr>> byShort;
        for (auto& c : active) {
            auto qn = c->klass->getQName();
            if (!qn) continue;
            byCanonical[qn->toCanonical()].push_back(c);
            byShort[qn->getTypeName()].push_back(c);
        }

        // Resolve a dependency from a name-qualifier-aware lookup.
        // Returns nullptr if no candidate matches; the caller turns
        // that into a missing-impl / ambiguous error with context.
        //
        // Three lookup tiers (A10):
        //   1. Direct match on the component's canonical name.
        //   2. Direct match on the component's short typeName.
        //   3. Implements-interface match — a component class whose
        //      getImplementedInterfaces() list contains the requested
        //      type. Lets `@Inject Persister p` resolve to a
        //      `@Component class Disk implements Persister`.
        auto resolveDependency =
            [&](const string& typeName, const string& nameQualifier,
                vector<ComponentDescriptorPtr>& outCandidates) -> ComponentDescriptorPtr {
                vector<ComponentDescriptorPtr> candidates;
                auto itC = byCanonical.find(typeName);
                if (itC != byCanonical.end()) {
                    candidates.insert(candidates.end(),
                        itC->second.begin(), itC->second.end());
                }
                if (candidates.empty()) {
                    auto itS = byShort.find(typeName);
                    if (itS != byShort.end()) {
                        candidates.insert(candidates.end(),
                            itS->second.begin(), itS->second.end());
                    }
                }
                if (candidates.empty()) {
                    // Interface walk. Linear over active components —
                    // typical compiles have a handful, so the constant
                    // factor is small. A short-circuit `break` after a
                    // match avoids double-counting when a class lists
                    // the same interface in its own implements clause
                    // plus inherits it from a superclass.
                    for (auto& c : active) {
                        if (!c->klass) continue;
                        for (auto& iface : c->klass->getImplementedInterfaces()) {
                            if (!iface || !iface->getQName()) continue;
                            if (iface->getQName()->toCanonical() == typeName
                                    || iface->getQName()->getTypeName() == typeName) {
                                candidates.push_back(c);
                                break;
                            }
                        }
                    }
                }
                outCandidates = candidates;
                if (candidates.empty()) return nullptr;
                if (!nameQualifier.empty()) {
                    ComponentDescriptorPtr named;
                    for (auto& c : candidates) {
                        if (c->name == nameQualifier) {
                            if (named) return nullptr;  // duplicate names — ambiguous
                            named = c;
                        }
                    }
                    return named;
                }
                if (candidates.size() == 1) return candidates.front();
                // Multiple candidates, no name on the @Inject site.
                // Prefer a single name-less candidate as the
                // unqualified default; otherwise ambiguous.
                ComponentDescriptorPtr unqualified;
                for (auto& c : candidates) {
                    if (c->name.empty()) {
                        if (unqualified) return nullptr;
                        unqualified = c;
                    }
                }
                return unqualified;
            };

        // ---- @Factory provider lookup (Unit 3) ----
        // An all-injected provider makes its product type injectable —
        // consumers @Inject the *product* and codegen calls the provider
        // (R3). An assisted provider does NOT: its product needs caller
        // args, so consumers inject the *factory* type itself (an
        // ordinary @Component, registered at discovery) and call it. A
        // provider participates only if its factory's selfComponent
        // survived profile / @TestComponent filtering.
        set<ComponentDescriptorPtr> activeSet(active.begin(), active.end());
        struct ProviderRef { FactoryDescriptorPtr f; int idx; };
        map<string, vector<ProviderRef>> provByCanonical, provByShort;
        for (auto& f : factoryClasses) {
            if (!f || !f->klass) continue;
            if (f->selfComponent && !activeSet.count(f->selfComponent)) continue;
            for (size_t i = 0; i < f->providers.size(); ++i) {
                auto& p = f->providers[i];
                if (p.hasAssisted) continue;
                if (!p.providedType || !p.providedType->getQName()) continue;
                provByCanonical[p.providedType->getQName()->toCanonical()]
                    .push_back({f, (int)i});
                provByShort[p.providedType->getQName()->getTypeName()]
                    .push_back({f, (int)i});
            }
        }

        // Unified resolution: a type may be provided by a @Component
        // ctor OR by an all-injected @Factory provider — never both
        // (that overlap is the @Factory/@Component ambiguity). Returns
        // exactly one of {component, factory-provider}, flags ambiguous,
        // or leaves both null for missing. Used for both @Inject fields
        // and all-injected providers' @Inject params.
        struct Resolution {
            ComponentDescriptorPtr component;
            FactoryDescriptorPtr factory;
            int providerIdx = -1;
            bool ambiguous = false;
        };
        auto resolveCombined =
            [&](const string& canonical, const string& shortName,
                const string& nameQualifier) -> Resolution {
                Resolution r;
                vector<ComponentDescriptorPtr> compCands;
                ComponentDescriptorPtr comp =
                    resolveDependency(canonical, nameQualifier, compCands);
                if (compCands.empty()) {
                    comp = resolveDependency(shortName, nameQualifier, compCands);
                }
                vector<ProviderRef> provCands;
                auto pit = provByCanonical.find(canonical);
                if (pit != provByCanonical.end()) provCands = pit->second;
                if (provCands.empty()) {
                    auto ps = provByShort.find(shortName);
                    if (ps != provByShort.end()) provCands = ps->second;
                }
                if (!compCands.empty() && !provCands.empty()) {
                    r.ambiguous = true;   // a @Component ctor AND a @Factory provide it
                    return r;
                }
                if (!provCands.empty()) {
                    if (provCands.size() > 1) { r.ambiguous = true; return r; }
                    r.factory = provCands[0].f;
                    r.providerIdx = provCands[0].idx;
                    return r;
                }
                r.component = comp;          // component-only (or nothing)
                if (!comp && !compCands.empty()) r.ambiguous = true;  // name-mismatch / multi-unnamed
                return r;
            };

        // Build per-component dependency edges and detect missing /
        // ambiguous resolution in one pass. The resolved (field →
        // target) pairs are written back to the descriptor's
        // resolvedFields list so A9 codegen has direct access to
        // them without re-running lookup.
        map<ComponentDescriptorPtr, vector<ComponentDescriptorPtr>> edges;
        for (auto& c : active) {
            edges[c];   // ensure every node is in the graph
            c->resolvedFields.clear();
            for (auto& [pname, prop] : c->klass->getProperties()) {
                if (!prop) continue;
                auto injectAnn = prop->findAnnotation("Inject");
                if (!injectAnn) continue;

                auto fieldType = prop->getType();
                if (!fieldType || !fieldType->getQName()) {
                    throw Exception(
                        "@Inject on field '" + prop->getName()
                            + "' of " + c->klass->getQName()->toCanonical()
                            + " has no resolvable type",
                        "CAJETA_ERROR_MISSING_COMPONENT");
                }
                const string& targetCanonical = fieldType->getQName()->toCanonical();
                const string& targetShort = fieldType->getQName()->getTypeName();
                const string nameQualifier = injectAnn->getString("name");
                bool isOptional = injectAnn->getBool("optional");

                // `allocate = ALLOCATE_X` is a bare identifier in
                // source. The visitor's classifyLiteral doesn't have
                // a "bare identifier as enumerator" shape, so it
                // falls through to String with the raw text (no
                // surrounding quotes — the captured strVal is just
                // the identifier). Read via getString first; for
                // forward-compat, also accept the ClassRef form
                // (`ALLOCATE_X.class`) if a user ever writes it.
                AllocateMode allocate = AllocateMode::Singleton;
                string allocStr = injectAnn->getString("allocate");
                if (allocStr.empty()) {
                    allocStr = injectAnn->getClassRef("allocate");
                }
                if (allocStr == "ALLOCATE_SINGLETON") {
                    allocate = AllocateMode::Singleton;
                } else if (allocStr == "ALLOCATE_OWNER_SCOPE") {
                    allocate = AllocateMode::OwnerScope;
                } else if (allocStr == "ALLOCATE_CALL_SCOPE") {
                    allocate = AllocateMode::CallScope;
                } else if (allocStr == "ALLOCATE_TRANSIENT") {
                    allocate = AllocateMode::Transient;
                } else if (!allocStr.empty()) {
                    // Recognized syntactic shape but not one of the
                    // four spec modes — flag with a missing-component
                    // -ish error so the user sees what's wrong.
                    throw Exception(
                        "@Inject on field '" + prop->getName()
                            + "' of " + c->klass->getQName()->toCanonical()
                            + " uses unknown allocate mode '" + allocStr + "'",
                        "CAJETA_ERROR_MISSING_COMPONENT");
                }
                if (allocate == AllocateMode::CallScope) {
                    // R5-A' implicit-function-body-scope integration
                    // hasn't reached the inject path yet. Reject with
                    // a clear "not yet" rather than silently fall
                    // through to Singleton.
                    throw Exception(
                        "@Inject on field '" + prop->getName()
                            + "' of " + c->klass->getQName()->toCanonical()
                            + " requests ALLOCATE_CALL_SCOPE which is "
                              "not yet supported (v1 ships SINGLETON, "
                              "OWNER_SCOPE, TRANSIENT)",
                        "CAJETA_ERROR_NOT_IMPLEMENTED");
                }

                Resolution res =
                    resolveCombined(targetCanonical, targetShort, nameQualifier);
                if (!res.component && !res.factory) {
                    if (res.ambiguous) {
                        throw Exception(
                            "Ambiguous @Inject of " + targetCanonical
                                + " in " + c->klass->getQName()->toCanonical()
                                + " — multiple @Component/@Factory providers (or a "
                                  "name qualifier matching none). Qualify the site.",
                            "CAJETA_ERROR_DI_AMBIGUOUS");
                    }
                    if (isOptional) {
                        // No candidate; field stays null. The codegen
                        // branch reads target==null and stores
                        // ConstantPointerNull.
                        ResolvedDependency rd;
                        rd.field = prop;
                        rd.target = nullptr;
                        rd.allocate = allocate;
                        rd.optional = true;
                        c->resolvedFields.push_back(rd);
                        continue;
                    }
                    throw Exception(
                        c->klass->getQName()->toCanonical()
                            + " needs " + targetCanonical
                            + ", but no @Component/@Factory provides " + targetCanonical
                            + " (active profile: " + activeProfile + ")",
                        "CAJETA_ERROR_MISSING_COMPONENT");
                }
                ResolvedDependency rd;
                rd.field = prop;
                rd.allocate = allocate;
                rd.optional = isOptional;
                if (res.factory) {
                    // Satisfied by an all-injected factory provider:
                    // the consumer depends on the factory's node.
                    rd.factory = res.factory;
                    rd.providerIdx = res.providerIdx;
                    edges[c].push_back(res.factory->selfComponent);
                } else {
                    rd.target = res.component;
                    edges[c].push_back(res.component);
                }
                c->resolvedFields.push_back(rd);
            }
        }

        // Resolve every provider's @Inject params (R1 edges) and
        // attribute their dependencies to the factory's node, so cycles
        // through factory params are caught by the DFS below. This runs
        // for assisted providers too: their product isn't a graph node,
        // but the @Inject params still need resolution for the
        // assisted-injection call-site splice (Unit 4b). Assisted (un-
        // marked) params are caller-supplied and skipped. A missing
        // @Inject-param target is a hard error.
        for (auto& f : factoryClasses) {
            if (!f || !f->klass || !f->selfComponent) continue;
            if (!activeSet.count(f->selfComponent)) continue;
            for (auto& p : f->providers) {
                for (auto& fp : p.params) {
                    if (!fp.injected) continue;
                    auto pType = fp.param ? fp.param->getType() : nullptr;
                    if (!pType || !pType->getQName()) {
                        throw Exception(
                            "@Inject factory param '"
                                + (fp.param ? fp.param->getName() : string("?"))
                                + "' of " + f->klass->getQName()->toCanonical()
                                + " has no resolvable type",
                            "CAJETA_ERROR_MISSING_COMPONENT");
                    }
                    Resolution pr = resolveCombined(
                        pType->getQName()->toCanonical(),
                        pType->getQName()->getTypeName(),
                        fp.nameQualifier);
                    if (!pr.component && !pr.factory) {
                        if (pr.ambiguous) {
                            throw Exception(
                                "Ambiguous @Inject param '" + fp.param->getName()
                                    + "' of " + f->klass->getQName()->toCanonical()
                                    + " — multiple @Component/@Factory providers.",
                                "CAJETA_ERROR_DI_AMBIGUOUS");
                        }
                        throw Exception(
                            f->klass->getQName()->toCanonical()
                                + " factory needs "
                                + pType->getQName()->toCanonical()
                                + ", but no @Component/@Factory provides it",
                            "CAJETA_ERROR_MISSING_COMPONENT");
                    }
                    if (pr.factory) {
                        fp.resolvedFactory = pr.factory;
                        fp.resolvedProviderIdx = pr.providerIdx;
                        edges[f->selfComponent].push_back(pr.factory->selfComponent);
                    } else {
                        fp.resolvedTarget = pr.component;
                        edges[f->selfComponent].push_back(pr.component);
                    }
                }
            }
        }

        // Cycle detection via colored DFS. WHITE = unvisited,
        // GRAY = on the current recursion path, BLACK = fully
        // explored. A GRAY edge means we re-entered an ancestor —
        // i.e., a cycle. Report the path from the cycle's entry to
        // the offending edge so the error names every participant.
        enum Color { WHITE, GRAY, BLACK };
        map<ComponentDescriptorPtr, Color> color;
        vector<ComponentDescriptorPtr> path;
        std::function<void(const ComponentDescriptorPtr&)> dfs =
            [&](const ComponentDescriptorPtr& node) {
                color[node] = GRAY;
                path.push_back(node);
                for (auto& dep : edges[node]) {
                    auto it = color.find(dep);
                    Color cc = (it == color.end()) ? WHITE : it->second;
                    if (cc == GRAY) {
                        // Cycle. Render from first occurrence of dep
                        // on path through the current node back to
                        // dep — that's the cycle proper.
                        string trace;
                        bool started = false;
                        for (auto& n : path) {
                            if (n == dep) started = true;
                            if (started) {
                                if (!trace.empty()) trace += " -> ";
                                trace += n->klass->getQName()->toCanonical();
                            }
                        }
                        trace += " -> " + dep->klass->getQName()->toCanonical();
                        throw Exception(
                            "Cycle in @Component dependency graph: " + trace,
                            "CAJETA_ERROR_DI_CYCLE");
                    }
                    if (cc == WHITE) dfs(dep);
                }
                color[node] = BLACK;
                path.pop_back();
            };
        for (auto& c : active) {
            if (color.find(c) == color.end()) dfs(c);
        }

        // A9 codegen prep: attach a synthesized __cajeta_inject()
        // static method to every active component. The method's
        // body emits the lazy-singleton pattern (alloc + ctor +
        // field injection + cache) at Phase 2 codegen, reading
        // resolvedFields populated above.
        //
        // The method registers on the class via addMethod, so
        // Phase 1's getLlvmFunctionType walk and Phase 2's
        // generateCode walk both pick it up without compiler
        // changes. The method's owning module is the component's
        // own module — multi-module DI graphs route each helper
        // into the right IR module.
        for (auto& c : active) {
            if (!c->klass) continue;
            // Skip if a manual __cajeta_inject already exists (idempotent
            // across repeated Compiler-resets in the same process).
            bool already = false;
            for (auto& [k, m] : c->klass->getMethods()) {
                if (m && m->getName() == "__cajeta_inject") {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            auto inject = std::make_shared<ComponentInjectMethod>(
                c->klass->getModule(), c->klass, c);
            c->klass->addMethod(inject);
        }

        // @Factory provider accessors (Unit 4a). One per all-injected
        // provider whose factory survived filtering. Synthesized AFTER
        // every component's __cajeta_inject exists (the accessor calls
        // them for the factory singleton + the providers' @Inject params)
        // and added to the factory class so the codegen worklist walks
        // it. Idempotent: skip if the accessor is already set.
        for (auto& f : factoryClasses) {
            if (!f || !f->klass || !f->selfComponent) continue;
            if (!activeSet.count(f->selfComponent)) continue;
            for (size_t i = 0; i < f->providers.size(); ++i) {
                auto& p = f->providers[i];
                if (p.hasAssisted) continue;            // assisted ⇒ no accessor (Unit 4b)
                if (p.accessor) continue;
                if (!p.providedType) continue;
                auto acc = std::make_shared<FactoryProviderMethod>(
                    f->klass->getModule(), f->klass, f, (int) i);
                f->klass->addMethod(acc);
                p.accessor = acc;
            }
        }
    }

    llvm::Function* CajetaModule::ensureFunctionVisible(
            llvm::IRBuilder<>* builder,
            llvm::Function* original,
            llvm::FunctionType* fnType) {
        if (!original || !builder) return original;
        llvm::BasicBlock* insertBB = builder->GetInsertBlock();
        if (!insertBB) return original;
        llvm::Function* enclosingFn = insertBB->getParent();
        if (!enclosingFn) return original;
        llvm::Module* callerLm = enclosingFn->getParent();
        if (!callerLm || callerLm == original->getParent()) {
            // Same module — no fixup needed.
            return original;
        }
        // Cross-module reference. Insert a declaration in the
        // caller's module; getOrInsertFunction returns either an
        // existing decl/def or a newly-created declaration. Cast
        // back to llvm::Function for use as a CallInst callee.
        llvm::FunctionCallee callee = callerLm->getOrInsertFunction(
            original->getName(), fnType);
        if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            return fn;
        }
        // Bitcast-shaped callee (different type signature found
        // under same name) — fall back to the original. The
        // resulting CreateCall will still verify under the original
        // function's module, just not the cross-module path. v1
        // shouldn't hit this case (cajeta-mangled names include
        // arg types in the canonical, so name + type are tightly
        // coupled).
        return original;
    }

    llvm::Function* CajetaModule::ensureFunctionInModule(
            llvm::Module* targetModule,
            llvm::Function* original) {
        if (!original || !targetModule) return original;
        if (original->getParent() == targetModule) return original;
        // Mirror ensureFunctionVisible's getOrInsertFunction call,
        // but key by the original's own llvm::FunctionType (no
        // IRBuilder, so no caller-side signature to reconcile).
        llvm::FunctionCallee callee = targetModule->getOrInsertFunction(
            original->getName(), original->getFunctionType());
        if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            return fn;
        }
        return original;
    }

    llvm::Constant* CajetaModule::ensureGlobalInModule(
            llvm::Module* targetModule,
            llvm::GlobalVariable* original) {
        if (!original || !targetModule) return original;
        if (original->getParent() == targetModule) return original;
        // getOrInsertGlobal returns the existing entry (decl or def)
        // when one with the same name is already present, or creates
        // a new declaration when absent. The declaration takes the
        // same ValueType so the merge step matches it to the donor's
        // definition.
        return llvm::cast<llvm::GlobalVariable>(targetModule->getOrInsertGlobal(
            original->getName(), original->getValueType()));
    }

    void CajetaModule::buildPendingPrototypes() {
        // Fixed-point loop: iterate canonicalMap, calling
        // tryGeneratePrototype on each not-yet-built class. As parents
        // fill in, dependent children become eligible on the next
        // iteration. Stop when a full pass actually built no new
        // prototypes.
        //
        // Progress is detected by checking isPrototypeBuilt before and
        // after each tryGeneratePrototype call rather than the call's
        // return value: tryGeneratePrototype returns true both when it
        // built fresh AND when the class was already built. Using the
        // before/after state instead avoids infinite-looping on
        // already-built classes.
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& [key, type] : CajetaType::getCanonicalMap()) {
                auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
                if (!klass) continue;
                if (klass->isPlaceholder()) continue;       // wait for fill-in
                if (klass->isDeclWalkInFlight()) continue;  // mid-declaration (nested materialize) — later sweep
                if (klass->isAnnotation()) continue;        // type token only, no layout
                if (klass->isPrototypeBuilt()) continue;
                if (klass->isTemplate()) continue;          // templates lay out only on instantiate
                klass->tryGeneratePrototype();
                if (klass->isPrototypeBuilt()) {
                    changed = true;
                }
            }
        }
    }

    void CajetaModule::validatePlaceholders() {
        for (auto& [key, type] : CajetaType::getCanonicalMap()) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
            if (klass && klass->isPlaceholder()) {
                throw Exception(
                    "unresolved forward reference to type '" + key
                        + "' — declared in the archive pre-scan but no "
                          "class declaration was visited that fills it in",
                    "CAJETA_ERROR_UNRESOLVED_PLACEHOLDER");
            }
        }
    }

    bool CajetaModule::linkRuntime() {
        // Tracked via presence of a sentinel runtime function in the module — if it's
        // already there, the runtime has been linked.
        if (llvmModule->getFunction("__cajeta_new_array") != nullptr) {
            return true;
        }
        llvm::StringRef bcRef(reinterpret_cast<const char*>(cajeta_runtime_bc), cajeta_runtime_bc_len);
        auto buf = llvm::MemoryBuffer::getMemBuffer(bcRef, "cajeta_runtime", /*RequiresNullTerminator=*/false);
        auto parsed = llvm::parseBitcodeFile(buf->getMemBufferRef(), *llvmContext);
        if (!parsed) {
            cerr << "cajeta: failed to parse embedded runtime bitcode: "
                 << llvm::toString(parsed.takeError()) << std::endl;
            return false;
        }
        std::unique_ptr<llvm::Module> rtModule = std::move(*parsed);
        // Align the runtime's target triple/datalayout with the user module's so the linker
        // doesn't complain about a mismatch.
        rtModule->setTargetTriple(llvmModule->getTargetTriple());
        rtModule->setDataLayout(llvmModule->getDataLayout());
        if (llvm::Linker::linkModules(*llvmModule, std::move(rtModule))) {
            cerr << "cajeta: Linker::linkModules failed when merging runtime" << std::endl;
            return false;
        }
        return true;
    }

    llvm::Module* CajetaModule::emitTargetLlvmModule() {
        // Emit≠resolution is a TEST-REUSE-only concern. In production / non-reuse
        // (no shared context) always return this module's own llvm::Module — the
        // historical behavior — so normal compiles are bit-for-bit unchanged and
        // never depend on currentEmitLlvmModule. In the reuse path, return the
        // function currently being emitted (set RAII by Method::generateCode) so
        // a stdlib-template instantiation's body IR + its runtime externs land in
        // the user emit module, not the cached stdlib. Null (outside a body) →
        // fall back to this module's own llvm::Module.
        if (Compiler::getSharedContext() && currentEmitLlvmModule)
            return currentEmitLlvmModule;
        return llvmModule;
    }

    llvm::Function* CajetaModule::getRuntimeFunction(const std::string& name,
                                                     llvm::Module* explicitTarget) {
        // Runtime definitions live exclusively in the compiler-owned
        // stdlib module (Compiler::ensureStdlibModule runs linkRuntime
        // once on it). User modules get module-local extern decls
        // referencing those definitions; the merge step (JIT linker
        // or AOT object link) resolves them.
        //
        // Bootstrap edge case: when this IS the stdlib module — or
        // when there is no stdlib module yet (Compiler ctor before
        // ensureStdlibModule runs) — fall back to linking the runtime
        // straight into this module so the lookup returns a usable
        // function on first call.
        // Target the module the builder is currently emitting into (the emit
        // module for a reuse-path instantiation), not necessarily this module —
        // so the runtime extern decl is co-resident with the calling function.
        // Guard against an out-of-codegen call (currentEmitLlvmModule null →
        // emitTargetLlvmModule falls back to this module's llvm module).
        // An explicit target overrides this: the caller knows the exact module its
        // IRBuilder is writing into (see header — drop-wrapper bodies on persistent
        // stdlib classes).
        llvm::Module* target = explicitTarget ? explicitTarget : emitTargetLlvmModule();
        auto stdlib = stdlibModule;
        if (!stdlib || stdlib.get() == this) {
            linkRuntime();
            llvm::Function* defFn = llvmModule->getFunction(name);
            if (!defFn) return nullptr;
            return ensureFunctionInModule(target, defFn);
        }
        llvm::Function* defFn = stdlib->getLlvmModule()->getFunction(name);
        if (!defFn) {
            // The named runtime helper isn't in the bitcode (or
            // hasn't been linked yet). Force a link on the stdlib
            // module's side and retry.
            stdlib->linkRuntime();
            defFn = stdlib->getLlvmModule()->getFunction(name);
            if (!defFn) return nullptr;
        }
        return ensureFunctionInModule(target, defFn);
    }

    llvm::Constant* CajetaModule::getOrCreateSourceFileConstant(const std::string& rawPath) {
        // Reproducible IR (docs-refactor 15.12.2): every IR-embedded source
        // path routes through --debug-prefix-map / the sourceRoot-relative
        // fallback, exactly like source_filename. Remap before the cache
        // lookup so the constants (and their dedup keys) are the
        // machine-independent form.
        const std::string path =
            remapSourcePath(rawPath, sourceRoot, compilerFlags.debugPrefixMap);
        auto it = sourceFileConstants.find(path);
        if (it != sourceFileConstants.end()) return it->second;

        // Emit as a private constant char array; return a ptr-to-first-element.
        // CreateGlobalStringPtr would also work, but it needs an active
        // IRBuilder, and this helper is callable from any codegen context
        // (including the function-entry-block builder that drop entries are
        // allocated through). Doing it via plain LLVM C++ APIs keeps the
        // call site free of builder-context concerns.
        auto& ctx = *llvmContext;
        llvm::Constant* strConst = llvm::ConstantDataArray::getString(ctx, path, /*AddNull=*/true);
        std::string globalName = ".cajeta.src." + std::to_string(sourceFileConstants.size());
        auto* gv = new llvm::GlobalVariable(
            *llvmModule, strConst->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strConst, globalName);
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        // The runtime helpers take `const char*`, i.e. a pointer to i8. The
        // global's type is an `[N x i8]` constant; bitcast to ptr.
        llvm::Constant* ptr = llvm::ConstantExpr::getBitCast(
            gv, llvm::PointerType::get(ctx, 0));
        sourceFileConstants[path] = ptr;
        return ptr;
    }

    // ── TBAA ───────────────────────────────────────────────────────────────
    void CajetaModule::ensureTbaaNodes() {
        if (tbaaCharType) {
            return;
        }
        llvm::MDBuilder mdb(*llvmContext);
        llvm::MDNode* root = mdb.createTBAARoot("Cajeta TBAA");
        // omnipotent char aliases everything; field + array-element descend from
        // it and are SIBLINGS, so they do not alias each other.
        tbaaCharType = mdb.createTBAAScalarTypeNode("omnipotent char", root);
        tbaaFieldType = mdb.createTBAAScalarTypeNode("cajeta field", tbaaCharType);
        tbaaArrayElemType =
            mdb.createTBAAScalarTypeNode("cajeta array element", tbaaCharType);
        tbaaFieldTag = mdb.createTBAAStructTagNode(tbaaFieldType, tbaaFieldType, 0);
        tbaaArrayElemTag =
            mdb.createTBAAStructTagNode(tbaaArrayElemType, tbaaArrayElemType, 0);
        tbaaCharTag = mdb.createTBAAStructTagNode(tbaaCharType, tbaaCharType, 0);
    }

    void CajetaModule::recordTbaaProvenance(llvm::Value* ptr, TbaaKind kind) {
        if (ptr && kind != TbaaKind::None) {
            tbaaProvenance[ptr] = kind;
        }
    }

    void CajetaModule::applyTbaaTags() {
        // Kill switch (debug / A-B attribution): CAJETA_TBAA_DISABLE=1 emits no
        // TBAA so the optimizer falls back to fully-conservative aliasing.
        if (tbaaProvenance.empty() || std::getenv("CAJETA_TBAA_DISABLE")) {
            return;
        }
        ensureTbaaNodes();
        for (llvm::Function& f : *llvmModule) {
            if (f.isDeclaration()) {
                continue;
            }
            for (llvm::BasicBlock& bb : f) {
                for (llvm::Instruction& inst : bb) {
                    llvm::Value* ptr = nullptr;
                    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                        ptr = ld->getPointerOperand();
                    } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                        ptr = st->getPointerOperand();
                    } else {
                        continue;
                    }
                    // Don't clobber metadata another path already set.
                    if (inst.getMetadata(llvm::LLVMContext::MD_tbaa)) {
                        continue;
                    }
                    auto it = tbaaProvenance.find(ptr->stripPointerCasts());
                    if (it == tbaaProvenance.end()) {
                        continue;
                    }
                    llvm::MDNode* tag = nullptr;
                    switch (it->second) {
                        case TbaaKind::Field:     tag = tbaaFieldTag; break;
                        case TbaaKind::ArrayElem: tag = tbaaArrayElemTag; break;
                        case TbaaKind::Char:      tag = tbaaCharTag; break;
                        default: break;
                    }
                    if (tag) {
                        inst.setMetadata(llvm::LLVMContext::MD_tbaa, tag);
                    }
                }
            }
        }
    }
}
