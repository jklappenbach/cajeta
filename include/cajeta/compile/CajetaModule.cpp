//
// Created by James Klappenbach on 10/22/22.
//

#include <utility>

#include <cajeta/compile/CajetaModule.h>
#include <cajeta/logging/CajetaLogger.h>
#include "cajeta/compile/Compiler.h"
#include "cajeta/type/StructureMetadata.h"
#include "cajeta/type/CajetaStructure.h"

namespace cajeta {
    map<QualifiedName*, CajetaModule*> CajetaModule::archive;

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
            string packageName = temp.substr(0, moduleNameIndex - 1);
            archivePath = temp + CAJETA_IR_EXTENSION;
            replace(packageName.begin(), packageName.end(), PATH_SEPARATOR, PACKAGE_SEPARATOR);
            qName = QualifiedName::getOrInsert(moduleName, packageName);

            llvmModule = new llvm::Module(qName->toCanonical(), *llvmContext);
            llvmModule->setSourceFileName(sourcePath);
            llvmModule->setDataLayout(targetMachine->createDataLayout());
            llvmModule->setTargetTriple(targetTriple);
            structureMetadata = new StructureMetadata(this);
        } else {
            cerr << "Error: Module srcPath must reference a cajeta module, a file with the correct naming convention";
        }
        archive[qName] = this;
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

    bool verifyImport(QualifiedName* qName) {
        return true;
    }

    void CajetaModule::processMetadata(CajetaStructure* structure) {
        structureMetadata->populateMetadataGlobals(structure);
    }

    void CajetaModule::onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx) {
        auto qName = QualifiedName::fromContext(ctx->qualifiedName());
        imports[qName->getTypeName()][qName->getPackageName()] = qName;
    }

    void CajetaModule::onStructureDeclaration(antlrcpp::Any any) {
        CajetaStructure* structure = any.as<CajetaStructure*>();
        structures.push_back(structure);
    }

    CajetaType* CajetaModule::getInitializerType() const {
        return initializerType;
    }

    void CajetaModule::setInitializerType(CajetaType* initializerType) {
        this->initializerType = initializerType;
    }

    llvm::Value* CajetaModule::getCurrentValue() const {
        return valueStack.back();
    }

    void CajetaModule::pushCurrentValue(llvm::Value* value) {
        valueStack.push_back(value);
    }

    llvm::Value* CajetaModule::popCurrentValue() {
        llvm::Value* value = valueStack.back();
        valueStack.pop_back();
        return value;
    }

    void CajetaModule::addToGenerate(Method* method) {
        toGenerate.push_back(method);
    }
}
