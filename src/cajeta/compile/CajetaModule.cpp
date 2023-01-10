//
// Created by James Klappenbach on 10/22/22.
//

#include <utility>

#include "CajetaModule.h"
#include "../logging/CajetaLogger.h"
#include "Compiler.h"
#include "../type/StructureMetadata.h"
#include "../type/CajetaClass.h"

namespace cajeta {
    map<string, CajetaModulePtr> CajetaModule::strutureToModule;
    map<string, CajetaModulePtr> CajetaModule::moduleVariables;

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
        CajetaClassPtr structure = std::any_cast<CajetaClassPtr>(any);
        structures[structure->toCanonical()] = structure;
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
}
