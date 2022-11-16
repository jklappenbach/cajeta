//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "cajeta/asn/AbstractSyntaxNode.h"
#include "llvm/IR/Module.h"
#include "cajeta/type/QualifiedName.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/type/Method.h"
#include <support/Any.h>
#include <string>
#include <fstream>
#include <filesystem>
#include <queue>
#include <llvm/Support/raw_os_ostream.h>

using namespace std;
using std::ofstream;

#define PATH_SEPARATOR              '/'
#define PACKAGE_SEPARATOR           '.'
#define CAJETA_EXTENSION            ".cajeta"
#define CAJETA_IR_EXTENSION         ".ll"

namespace cajeta {
    class CajetaModule {
    private:
        static map<QualifiedName*, CajetaModule*> archive;
        static map<string, Method*> methodArchive;
        map<string, map<string, QualifiedName*>> imports;
        QualifiedName* qName;
        string sourcePath;
        string sourceRoot;
        string archiveRoot;
        string archivePath;

        list<CajetaStructure*> structureStack;
        list<CajetaStructure*> structures;
        Method* currentMethod;

        // Current state
        llvm::Module* llvmModule;
        list<llvm::Value*> valueStack;
        list<string> identifierStack;
        list<Field*> fieldStack;
        llvm::IRBuilder<>* builder;
        llvm::LLVMContext* llvmContext;
        llvm::TargetMachine* targetMachine;
        CajetaType* initializerType;
        string targetTriple;


    public:
        CajetaModule(llvm::LLVMContext* llvmContext,
                     string sourcePath,
                     string sourceRoot,
                     string archiveRoot,
                     string targetTriple,
                     llvm::TargetMachine* targetMachine);

        QualifiedName* getQName() const {
            return qName;
        }

        void setQName(QualifiedName* qName) {
            CajetaModule::qName = qName;
        }

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

        CajetaType* getInitializerType() const;

        void setInitializerType(CajetaType* initializerType);

        llvm::Value* getCurrentValue() const;

        void pushCurrentValue(llvm::Value* value);

        llvm::Value* popCurrentValue();

        static const map<QualifiedName*, CajetaModule*>& getTypeArchive() {
            return archive;
        }

        static void setArchive(const map<QualifiedName*, CajetaModule*>& archive) {
            CajetaModule::archive = archive;
        }

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

        map<string, map<string, QualifiedName*>>& getImports() {
            return imports;
        }

        list<CajetaStructure*>& getStructureStack() {
            return structureStack;
        }

        list<llvm::Value*>& getValueStack() {
            return valueStack;
        }

        list<Field*>& getFieldStack() {
            return fieldStack;
        }

        list<CajetaStructure*>& getStructureList() { return structures; }

        llvm::IRBuilder<>* getBuilder() const;

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

        void setCurrentMethod(Method* method) {
            this->currentMethod = method;
        }

        Method* getCurrentMethod() {
            return currentMethod;
        }

        unique_ptr<list<Method*>> getAllMethods() {
            unique_ptr<list<Method*>> result(new list<Method*>);
            for (CajetaStructure* structure : structures) {
                for (auto methodEntry : structure->getMethods()) {
                    result->push_back(methodEntry.second);
                }
            }
            return result;
        }

        void onPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx);

        void onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx);
        void onStructureDeclaration(antlrcpp::Any any);
    };
}

