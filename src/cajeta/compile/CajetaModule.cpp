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
    thread_local map<string, MethodPtr> CajetaModule::methods;
    thread_local map<string, CajetaModulePtr> CajetaModule::strutureToModule;
    thread_local CajetaModulePtr CajetaModule::activeModule;
    thread_local CajetaModulePtr CajetaModule::currentCodegenModule;
    thread_local CajetaModulePtr CajetaModule::reuseEmitModule;
    thread_local CajetaModulePtr CajetaModule::activeUnitModule;
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

    // Builds a module for a synthetic unit named by qName: creates the
    // llvm::Module and pins it to the target's data layout and triple.
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
        // LLVM 21 narrowed Module::setTargetTriple to llvm::Triple; 18 / 20
        // take the triple string, which the Triple ctor accepts in both.
#if LLVM_VERSION_MAJOR >= 21
        llvmModule->setTargetTriple(llvm::Triple(targetTriple));
#else
        llvmModule->setTargetTriple(targetTriple);
#endif
    }

    // Builds a module from a source file, deriving package and module name from
    // sourcePath relative to sourceRoot; a path not ending in ".cajeta" errors.
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

        // The extension must match at the TAIL: the FIRST ".cajeta" in the path
        // may belong to a directory, which sends the package math off it.
        const std::string kExt = CAJETA_EXTENSION;
        int suffixIndex = -1;
        if (sourcePath.size() > kExt.size()
            && sourcePath.compare(sourcePath.size() - kExt.size(),
                                  kExt.size(), kExt) == 0) {
            suffixIndex = (int) (sourcePath.size() - kExt.size());
        }
        if (suffixIndex >= 0) {
            // Whether `temp` opens with PATH_SEPARATOR depends on whether the
            // caller's sourceRoot ended in one; normalize so the math does not.
            string temp = sourcePath.substr(sourceRoot.size(), suffixIndex - sourceRoot.size());
#ifdef _WIN32
            // Package derivation keys off PATH_SEPARATOR, and backslash is never
            // a valid Windows filename character, so this is lossless.
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
#if LLVM_VERSION_MAJOR >= 21
        llvmModule->setTargetTriple(llvm::Triple(targetTriple));
#else
        llvmModule->setTargetTriple(targetTriple);
#endif
        } else {
            cerr << "Error: Module srcPath must reference a code pModule, a file with the correct naming convention";
        }
    }

    // Maps a source path to the machine-independent form embedded in IR:
    // --debug-prefix-map=<from>=<to> when it matches, else sourceRoot-relative.
    std::string CajetaModule::remapSourcePath(const std::string& sourcePath,
                                              const std::string& sourceRoot,
                                              const std::string& debugPrefixMap) {
        auto normalize = [](std::string s) {
            std::replace(s.begin(), s.end(), '\\', '/');
            return s;
        };
        std::string path = normalize(sourcePath);

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

    // Rewrites the llvm module's source_filename through remapSourcePath so the
    // emitted IR carries no machine-specific path.
    void CajetaModule::canonicalizeSourceFileName() {
        if (sourcePath.empty()) {
            return;
        }
        llvmModule->setSourceFileName(
            remapSourcePath(sourcePath, sourceRoot, compilerFlags.debugPrefixMap));
    }

    // Records that `triggering` demanded a template instantiation owned by
    // another module - only those can vanish when its codegen is skipped.
    void CajetaModule::noteCrossModuleInstantiation(
        const CajetaModulePtr& triggering, const CajetaClassPtr& inst) {
        if (!triggering || !inst) {
            return;
        }
        if (inst->getModule() == triggering) {
            return;
        }
        triggering->instantiationObligations.insert(inst->toCanonical());
    }

    // Records a cross-module method instantiation on `triggering`, keyed by
    // getMapKey(false); the `::` in that key marks it as a method obligation.
    void CajetaModule::noteCrossModuleMethodInstantiation(
        const CajetaModulePtr& triggering, const MethodPtr& inst) {
        if (!triggering || !inst) {
            return;
        }
        if (inst->getModule() == triggering) {
            return;
        }
        triggering->instantiationObligations.insert(inst->getMapKey(false));
    }

    // Records that `triggering` read a static field owned by another module,
    // as "<owner canonical>::<field>".
    void CajetaModule::noteCrossModuleStaticFieldRef(
        const CajetaModulePtr& triggering, const CajetaClassPtr& owner,
        const std::string& fieldName) {
        if (!triggering || !owner || fieldName.empty()) {
            return;
        }
        if (owner->getModule() == triggering) {
            return;
        }
        triggering->instantiationObligations.insert(
            owner->toCanonical() + "::" + fieldName);
    }

    // Writes the obligations beside the emitted IR at <archive>.obligations, one
    // per line in sorted order; an empty set removes a stale sidecar instead.
    void CajetaModule::writeObligationsSidecar() const {
        std::string path = archiveRoot + archivePath;
        const std::string irExt = CAJETA_IR_EXTENSION;
        if (path.size() >= irExt.size() &&
            path.compare(path.size() - irExt.size(), irExt.size(), irExt) == 0) {
            path.replace(path.size() - irExt.size(), irExt.size(), ".obligations");
        } else {
            path += ".obligations";
        }
        std::error_code ec;
        if (instantiationObligations.empty()) {
            std::filesystem::remove(path, ec);
            return;
        }
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        for (const auto& name : instantiationObligations) {
            out << name << "\n";
        }
    }

    // Writes the obligations to the incremental-cache slot. Unlike the sidecar an
    // empty set still writes an empty file: an absent slot means "never built".
    bool CajetaModule::writeObligationsToSlot() const {
        if (cacheObligationsSlot.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(cacheObligationsSlot).parent_path(), ec);
        std::ofstream out(cacheObligationsSlot, std::ios::trunc);
        if (!out) return false;
        for (const auto& name : instantiationObligations) {
            out << name << "\n";
        }
        return static_cast<bool>(out);
    }

    // Writes this module's bitcode to the cache slot through a same-directory
    // temp file renamed over it, so a reader never sees a partial write.
    bool CajetaModule::writeBitcodeToSlot() const {
        if (cacheBcSlot.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(cacheBcSlot).parent_path(), ec);
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

    // Replaces this module's llvm::Module with the bitcode in the cache slot;
    // false when the slot is missing or unparseable, module left untouched.
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
        // Every cached llvm handle below points INTO the module just deleted, and
        // codegen still reaches a swapped module, so drop them. The tbaa MDNode*
        // members stay: metadata is owned by the LLVMContext, not the module.
        sourceFileConstants.clear();
        tbaaProvenance.clear();
        return true;
    }

    llvm::IRBuilder<>* CajetaModule::getBuilder() const {
        return builder;
    }

    // Allocates `ty` in the enclosing function's entry block so it runs once on
    // entry however deep the declaration sits. Returns null when there is no
    // builder - a --lint resolve, whose caller wants a slotless field.
    llvm::AllocaInst* CajetaModule::createEntryAlloca(llvm::Type* ty, const std::string& name) {
        if (!builder) return nullptr;
        llvm::BasicBlock* insertBB = builder->GetInsertBlock();
        llvm::Function* fn = insertBB ? insertBB->getParent() : nullptr;
        if (fn) {
            llvm::BasicBlock& entry = fn->getEntryBlock();
            llvm::IRBuilder<> eb(&entry, entry.begin());
            return eb.CreateAlloca(ty, nullptr, name);
        }
        return builder->CreateAlloca(ty, nullptr, name);
    }

    // Records the unit's package and checks it against the path the module was
    // built from. A script unit may omit the declaration, so a null ctx is legal.
    void CajetaModule::onPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx) {
        if (ctx == nullptr) return;
        std::vector<CajetaParser::IdentifierContext*> identifiers = ctx->qualifiedName()->identifier();
        auto itr = identifiers.begin();
        string packageName = (*itr)->getText();
        itr++;
        while (itr != identifiers.end()) {
            packageName.append(".");
            packageName.append((*itr)->getText());
            itr++;
        }

        if (qName->getPackageName() != packageName && !scriptUnit) {
            // Under lint the disk path is not authoritative - a staged buffer's
            // temp path cannot match its declared package - so skip the check.
            if (DiagnosticEngine::active()) return;
            string message = "Declared package name " + packageName + " must match the compilation unit path of " +
                qName->getPackageName();
            CajetaLogger::log(ERROR, ctx, sourcePath, "CAJETA_ERROR_PACKAGE_MISMATCH", message);
        }
    }

    bool verifyImport(QualifiedNamePtr qName) {
        return true;
    }

    void CajetaModule::processMetadata(CajetaClassPtr structure) {
        structureMetadata->populate(structure);
    }

    // Registers an import, records its xref edge and runs the lazy-stdlib hook;
    // imports resolve here, not in CajetaType::fromContext, so nothing else does.
    void CajetaModule::onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) {
        auto qName = QualifiedName::fromContext(ctx->qualifiedName());
        imports[qName->getTypeName()][qName->getPackageName()] = qName;
        // At the LEAF identifier, the name a developer Ctrl-clicks; --emit-xref only.
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
        // An import of an on-demand package (cajeta.math) triggers its prescan.
        if (stdlibImportHook) {
            stdlibImportHook(qName->getPackageName());
        }
    }

    // Registers a declared type. A declaration that yields no CajetaClass (an
    // enum registers its constants elsewhere) hands back an empty `any`.
    void CajetaModule::onStructureDeclaration(std::any any) {
        if (!any.has_value()) return;
        try {
            CajetaClassPtr structure = std::any_cast<CajetaClassPtr>(any);
            if (structure) {
                structures[structure->toCanonical()] = structure;
            }
        } catch (const std::bad_any_cast&) {
            // Not a CajetaClass; nothing to register here.
        }
    }

    // Synthesizes the interface vtables deferred while a class was a placeholder.
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

    // Creates a module for sourcePath and registers it by canonical name.
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

    // Clears every per-compile registry, the method archive and the module pins.
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
        // Snapshot of the module-level registries. Holds the stdlib singleton so a
        // restore re-pins it, and the method archive so forward refs reset with it.
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
        // A second slot, taken after the lint server's sibling sweep and restored
        // independently of the pristine baseline on a warm request.
        ModuleGlobalsBaseline g_moduleContextBaseline;

        // Fills `b` from the registries passed in plus the current method archive.
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

    // ---- Registry baselines: pristine post-stdlib, and warm lint-server context.
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

    // Pushes an AdviceMatch onto every method an aspect's pointcut selects, then
    // orders them by @Order. A pointcut that names a registered class is
    // type-based; any other name is a marker annotation.
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

        set<CajetaClassPtr> aspectSet(aspectClasses.begin(), aspectClasses.end());

        auto resolveClassByShortName =
            [](const string& shortName) -> CajetaClassPtr {
                for (auto& [canonical, mod] : strutureToModule) {
                    if (!mod) continue;
                    auto& structs = mod->getStructures();
                    auto it = structs.find(canonical);
                    if (it == structs.end() || !it->second) continue;
                    if (it->second->isAnnotation()) continue;
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
                if (!ann) continue;
                string pointcutClassName = ann->getClassRef();
                if (pointcutClassName.empty()) {
                    continue;
                }

                CajetaClassPtr pointcutClass = resolveClassByShortName(pointcutClassName);
                PointcutShape shape = pointcutClass
                    ? PointcutShape::Type
                    : PointcutShape::MarkerAnnotation;

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
                            // A type pointcut selects the class itself and every
                            // descendant: walk supers plus implemented interfaces.
                            if (userClass == pointcutClass) {
                                matched = true;
                            } else {
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

        // Advice with no @Order sorts last, declaration order preserved by the
        // stable_sort; the emit helpers walk this vector, so it fixes firing order.
        auto readOrder = [](const AdviceMatch& m) -> int64_t {
            if (!m.adviceMethod) return INT64_MAX;
            auto ann = m.adviceMethod->findAnnotation("Order");
            if (!ann) return INT64_MAX;
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

    // Filters components by active profile, applies @TestComponent overrides,
    // resolves every @Inject site, rejects cycles, then attaches the synthesized
    // __cajeta_inject and factory accessors. Emits no IR itself.
    void CajetaModule::resolveDependencyGraph() {
        if (componentClasses.empty()) return;

        auto profileMatches = [](const ComponentDescriptorPtr& c) {
            if (c->profiles.empty()) return true;
            for (auto& p : c->profiles) {
                if (p == activeProfile) return true;
            }
            return false;
        };

        const bool testMode = (activeProfile == "test");

        vector<ComponentDescriptorPtr> active;
        // A @TestComponent masks every non-test @Component sharing one of its
        // interfaces; with no interface it masks nothing (own concrete type only).
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

        // Keyed by canonical AND short name, so a consumer can write
        // `@Inject Database db;` without the package.
        map<string, vector<ComponentDescriptorPtr>> byCanonical;
        map<string, vector<ComponentDescriptorPtr>> byShort;
        for (auto& c : active) {
            auto qn = c->klass->getQName();
            if (!qn) continue;
            byCanonical[qn->toCanonical()].push_back(c);
            byShort[qn->getTypeName()].push_back(c);
        }

        // Resolves by canonical name, then short name, then by implemented
        // interface. Null when nothing matches or the choice stays ambiguous.
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
                            if (named) return nullptr;
                            named = c;
                        }
                    }
                    return named;
                }
                if (candidates.size() == 1) return candidates.front();
                ComponentDescriptorPtr unqualified;
                for (auto& c : candidates) {
                    if (c->name.empty()) {
                        if (unqualified) return nullptr;
                        unqualified = c;
                    }
                }
                return unqualified;
            };

        // An all-injected provider makes its product type injectable; an assisted
        // one does not - its product needs caller args, so consumers inject the
        // factory itself, an ordinary @Component, and call it.
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

        // A type may come from a @Component ctor OR an all-injected @Factory
        // provider, never both - that overlap is the ambiguity this flags.
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
                    r.ambiguous = true;
                    return r;
                }
                if (!provCands.empty()) {
                    if (provCands.size() > 1) { r.ambiguous = true; return r; }
                    r.factory = provCands[0].f;
                    r.providerIdx = provCands[0].idx;
                    return r;
                }
                r.component = comp;
                if (!comp && !compCands.empty()) r.ambiguous = true;
                return r;
            };

        // Resolved (field -> target) pairs are written back to resolvedFields, so
        // codegen needs no second lookup.
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

                // `allocate = ALLOCATE_X` is a bare identifier: the visitor has no
                // enumerator shape, so it arrives as a String holding the raw text.
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
                    throw Exception(
                        "@Inject on field '" + prop->getName()
                            + "' of " + c->klass->getQName()->toCanonical()
                            + " uses unknown allocate mode '" + allocStr + "'",
                        "CAJETA_ERROR_MISSING_COMPONENT");
                }
                if (allocate == AllocateMode::CallScope) {
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

        // Provider @Inject params are attributed to the factory's node, so cycles
        // through them are caught below; assisted (unmarked) params are skipped.
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

        // Colored DFS: an edge to a GRAY node re-enters an ancestor, i.e. a cycle.
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

        // __cajeta_inject() emits the lazy-singleton pattern at codegen from
        // resolvedFields. It is added to the component's OWN module, so a
        // multi-module graph routes each helper into the right IR module.
        for (auto& c : active) {
            if (!c->klass) continue;
            // Idempotent across repeated Compiler resets in one process.
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

        // One accessor per all-injected provider, synthesized after every
        // __cajeta_inject exists: it calls them for the factory singleton and for
        // the provider's own @Inject params.
        for (auto& f : factoryClasses) {
            if (!f || !f->klass || !f->selfComponent) continue;
            if (!activeSet.count(f->selfComponent)) continue;
            for (size_t i = 0; i < f->providers.size(); ++i) {
                auto& p = f->providers[i];
                if (p.hasAssisted) continue;
                if (p.accessor) continue;
                if (!p.providedType) continue;
                auto acc = std::make_shared<FactoryProviderMethod>(
                    f->klass->getModule(), f->klass, f, (int) i);
                f->klass->addMethod(acc);
                p.accessor = acc;
            }
        }
    }

    // Returns `original` made referenceable from the module the builder is
    // writing into, declaring it there when the call crosses modules.
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
            return original;
        }
        llvm::FunctionCallee callee = callerLm->getOrInsertFunction(
            original->getName(), fnType);
        if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            return fn;
        }
        // A bitcast-shaped callee means the name resolved to another signature;
        // mangled names embed arg types, so this should be unreachable.
        fprintf(stderr,
            "cajeta: ensureFunctionVisible CROSS-MODULE fallback: %s "
            "(caller module %s, original module %s)\n",
            original->getName().str().c_str(),
            callerLm->getName().str().c_str(),
            original->getParent()->getName().str().c_str());
        return original;
    }

    // Returns `original` declared in targetModule, keyed by its own FunctionType
    // (no builder here, so no caller-side signature to reconcile).
    llvm::Function* CajetaModule::ensureFunctionInModule(
            llvm::Module* targetModule,
            llvm::Function* original) {
        if (!original || !targetModule) return original;
        if (original->getParent() == targetModule) return original;
        llvm::FunctionCallee callee = targetModule->getOrInsertFunction(
            original->getName(), original->getFunctionType());
        if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            return fn;
        }
        return original;
    }

    // Returns `original` declared in targetModule; the declaration takes the
    // donor's ValueType so the merge step matches it to the definition.
    llvm::Constant* CajetaModule::ensureGlobalInModule(
            llvm::Module* targetModule,
            llvm::GlobalVariable* original) {
        if (!original || !targetModule) return original;
        if (original->getParent() == targetModule) return original;
        return llvm::cast<llvm::GlobalVariable>(targetModule->getOrInsertGlobal(
            original->getName(), original->getValueType()));
    }

    // Builds class prototypes to a fixed point: a class becomes eligible once its
    // parents fill in. Progress is read from isPrototypeBuilt before and after
    // each call, not the return value, which is also true for built classes.
    void CajetaModule::buildPendingPrototypes() {
        bool changed = true;
        while (changed) {
            changed = false;
            // Completing a deferred instantiation can both free new classes and
            // defer more of its own, so it belongs inside the fixpoint.
            if (CajetaClass::drainDeferredInstantiations()) {
                changed = true;
            }
            for (auto& [key, type] : CajetaType::getCanonicalMap()) {
                auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
                if (!klass) continue;
                if (klass->isPlaceholder()) continue;
                if (klass->isDeclWalkInFlight()) continue;  // mid-declaration (nested materialize) — later sweep
                if (klass->isAnnotation()) continue;
                if (klass->isPrototypeBuilt()) continue;
                if (klass->isTemplate()) continue;          // templates lay out only on instantiate
                klass->tryGeneratePrototype();
                if (klass->isPrototypeBuilt()) {
                    changed = true;
                }
            }
        }
    }

    // Throws if any pre-scanned forward reference was never filled in.
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

    // Links the embedded runtime bitcode into this module. Idempotent: the
    // presence of __cajeta_new_array means it is already linked.
    bool CajetaModule::linkRuntime() {
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
        // Match the user module's triple/datalayout or the linker rejects the merge.
        rtModule->setTargetTriple(llvmModule->getTargetTriple());
        rtModule->setDataLayout(llvmModule->getDataLayout());
        if (llvm::Linker::linkModules(*llvmModule, std::move(rtModule))) {
            cerr << "cajeta: Linker::linkModules failed when merging runtime" << std::endl;
            return false;
        }
        return true;
    }

    // Returns the llvm::Module codegen should emit into: this module's own, except
    // on the shared-context reuse path, where a stdlib-template body and its
    // runtime externs must land in the module being emitted.
    llvm::Module* CajetaModule::emitTargetLlvmModule() {
        if (Compiler::getSharedContext() && currentEmitLlvmModule)
            return currentEmitLlvmModule;
        return llvmModule;
    }

    // Returns a decl of runtime function `name` in explicitTarget, or in the module
    // codegen is emitting into. Definitions live only in the stdlib module; when
    // this IS that module, or none exists yet, the runtime links in here instead.
    llvm::Function* CajetaModule::getRuntimeFunction(const std::string& name,
                                                     llvm::Module* explicitTarget) {
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
            stdlib->linkRuntime();
            defFn = stdlib->getLlvmModule()->getFunction(name);
            if (!defFn) return nullptr;
        }
        return ensureFunctionInModule(target, defFn);
    }

    // Interns `rawPath` as a private constant string and returns a ptr to it. The
    // path is remapped before the cache lookup, so the constant and its dedup key
    // are both the machine-independent form.
    llvm::Constant* CajetaModule::getOrCreateSourceFileConstant(const std::string& rawPath) {
        const std::string path =
            remapSourcePath(rawPath, sourceRoot, compilerFlags.debugPrefixMap);
        auto it = sourceFileConstants.find(path);
        if (it != sourceFileConstants.end()) return it->second;

        // Built through plain LLVM APIs, not CreateGlobalStringPtr: this is called
        // from codegen contexts that have no active IRBuilder.
        auto& ctx = *llvmContext;
        llvm::Constant* strConst = llvm::ConstantDataArray::getString(ctx, path, /*AddNull=*/true);
        std::string globalName = ".cajeta.src." + std::to_string(sourceFileConstants.size());
        auto* gv = new llvm::GlobalVariable(
            *llvmModule, strConst->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strConst, globalName);
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        llvm::Constant* ptr = llvm::ConstantExpr::getBitCast(
            gv, llvm::PointerType::get(ctx, 0));
        sourceFileConstants[path] = ptr;
        return ptr;
    }

    // ── TBAA ───────────────────────────────────────────────────────────────
    // Creates this module's TBAA type and tag nodes once.
    void CajetaModule::ensureTbaaNodes() {
        if (tbaaCharType) {
            return;
        }
        llvm::MDBuilder mdb(*llvmContext);
        llvm::MDNode* root = mdb.createTBAARoot("Cajeta TBAA");
        // Field and array-element descend from omnipotent char as SIBLINGS, so a
        // field access and an element access never alias each other.
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

    // Tags every load/store whose pointer has recorded provenance and no tag yet.
    void CajetaModule::applyTbaaTags() {
        // Kill switch for A-B attribution: CAJETA_TBAA_DISABLE=1 emits no TBAA.
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
