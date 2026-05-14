//
// Created by James Klappenbach on 10/22/22.
//

#include <utility>

#include "CajetaModule.h"
#include "../logging/CajetaLogger.h"
#include "Compiler.h"
#include "../method/Method.h"
#include "../type/StructureMetadata.h"
#include "../type/CajetaClass.h"
#include "../runtime/EmbeddedRuntime.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"

namespace cajeta {
    map<string, MethodPtr> CajetaModule::methods;
    map<string, CajetaModulePtr> CajetaModule::strutureToModule;
    CajetaModulePtr CajetaModule::activeModule;
    map<string, CajetaModulePtr> CajetaModule::moduleVariables;
    vector<CajetaClassPtr> CajetaModule::aspectClasses;

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
            string temp = sourcePath.substr(sourceRoot.size(), suffixIndex - sourceRoot.size());
            int moduleNameIndex = temp.rfind(PATH_SEPARATOR) + 1;
            string moduleName = temp.substr(moduleNameIndex, suffixIndex);
            string packageName = temp.substr(1, moduleNameIndex - 2);
            archivePath = temp + CAJETA_IR_EXTENSION;
            replace(packageName.begin(), packageName.end(), PATH_SEPARATOR, PACKAGE_SEPARATOR);
            qName = QualifiedName::getOrInsert(moduleName, packageName);

            llvmModule = new llvm::Module(qName->toCanonical(), *llvmContext);
            llvmModule->setSourceFileName(sourcePath);
            llvmModule->setDataLayout(targetMachine->createDataLayout());
            llvmModule->setTargetTriple(targetTriple);
        } else {
            cerr << "Error: Module srcPath must reference a code pModule, a file with the correct naming convention";
        }
    }

    llvm::IRBuilder<>* CajetaModule::getBuilder() const {
        return builder;
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
        Method::getArchive().clear();
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
    // with this name") — annotation types declared via `@interface`
    // don't register as classes today (visitAnnotationTypeDeclaration
    // is inert), so absence-from-structures is a reliable proxy.
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
                            // Type-based pointcut: the method belongs
                            // to the pointcut class. Inheritance walks
                            // (the doc's "implementer-of-interface"
                            // case) ship alongside A12 once the
                            // ancestor-resolution path is stable for
                            // post-vtable use.
                            if (userClass == pointcutClass) {
                                matched = true;
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

    llvm::Function* CajetaModule::getRuntimeFunction(const std::string& name) {
        linkRuntime();
        return llvmModule->getFunction(name);
    }
}
