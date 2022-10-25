//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "llvm/IR/Module.h"
#include "cajeta/module/QualifiedName.h"
#include "cajeta/type/CajetaType.h"
#include <string>

namespace cajeta {

#define PATH_SEPARATOR              '/'
#define PACKAGE_SEPARATOR           '.'
#define CAJETA_EXTENSION            ".cajeta"
#define CAJETA_IR_EXTENSION         ".ll"

    class CompilationUnit {
    private:
        static map<QualifiedName*, CompilationUnit*> archive;
        string sourcePath;
        string archiveRoot;
        string archivePath;
        QualifiedName* qName;
        llvm::Module* module;
        llvm::TargetMachine* targetMachine;
        string targetTriple;
        CajetaType* cejetaType;

        map<string, map<string, QualifiedName*>> imports;
        CompilationUnit(llvm::LLVMContext* ctxLlvm,
                        string srcPath,
                        string sourceRoot,
                        string archiveRoot,
                        llvm::TargetMachine* targetMachine,
                        string targetTriple) {
            int suffixIndex = srcPath.find(CAJETA_EXTENSION);
            if (suffixIndex >= 0) {
                this->sourcePath = srcPath;
                this->archiveRoot = archiveRoot;
                string temp = srcPath.substr(sourceRoot.size(), suffixIndex - sourceRoot.size());
                int moduleNameIndex = temp.rfind(PATH_SEPARATOR) + 1;
                string moduleName = temp.substr(moduleNameIndex, suffixIndex);
                string packageName = temp.substr(0, moduleNameIndex - 1);
                archivePath = temp + CAJETA_IR_EXTENSION;
                replace(packageName.begin(), packageName.end(), PATH_SEPARATOR, PACKAGE_SEPARATOR);
                qName = QualifiedName::create(moduleName, packageName);
                module = new llvm::Module(qName->toString(), *ctxLlvm);
                module->setDataLayout(targetMachine->createDataLayout());
                module->setTargetTriple(targetTriple);
            } else {
                cerr << "Error: Module srcPath must reference a cajeta module, a file with the correct naming convention";
            }
        }
    public:

        QualifiedName* getQName() const {
            return qName;
        }

        void setQName(QualifiedName* qName) {
            CompilationUnit::qName = qName;
        }

        llvm::Module* getModule() const {
            return module;
        }

        void setModule(llvm::Module* module) {
            CompilationUnit::module = module;
        }

        static const map<QualifiedName*, CompilationUnit*>& getArchive() {
            return archive;
        }

        static void setArchive(const map<QualifiedName*, CompilationUnit*>& archive) {
            CompilationUnit::archive = archive;
        }

        const string& getSourcePath() const {
            return sourcePath;
        }

        void setSourcePath(const string& sourcePath) {
            CompilationUnit::sourcePath = sourcePath;
        }

        const string& getArchiveRoot() const {
            return archiveRoot;
        }

        void setArchiveRoot(const string& archiveRoot) {
            CompilationUnit::archiveRoot = archiveRoot;
        }

        const string& getArchivePath() const {
            return archivePath;
        }

        void setArchivePath(const string& archivePath) {
            CompilationUnit::archivePath = archivePath;
        }

        llvm::TargetMachine* getTargetMachine() const {
            return targetMachine;
        }

        void setTargetMachine(llvm::TargetMachine* targetMachine) {
            CompilationUnit::targetMachine = targetMachine;
        }

        const string& getTargetTriple() const {
            return targetTriple;
        }

        void setTargetTriple(const string& targetTriple) {
            CompilationUnit::targetTriple = targetTriple;
        }

        CajetaType* getCejetaType() const {
            return cejetaType;
        }

        void setCejetaType(CajetaType* cejetaType) {
            CompilationUnit::cejetaType = cejetaType;
        }

        map<string, map<string, QualifiedName*>>& getImports() {
            return imports;
        }

        void setImports(const map<string, map<string, QualifiedName*>>& imports) {
            CompilationUnit::imports = imports;
        }

        static CompilationUnit* create(llvm::LLVMContext* ctxLlvm,
                                       string path,
                                       string sourceRoot,
                                       string archiveRoot,
                                       llvm::TargetMachine* targetMachine,
                                       string targetTriple);
        void writeIRFileTarget() {
            string targetPath = archiveRoot + "/" + archivePath;
            std::error_code ec;
            llvm::raw_fd_ostream ostream(targetPath, ec, llvm::sys::fs::OF_None);
            llvm::WriteBitcodeToFile(*this->module, ostream);
        }
    };
}

