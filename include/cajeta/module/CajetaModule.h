//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "llvm/IR/Module.h"
#include "cajeta/module/QualifiedName.h"

namespace cajeta {
    class CajetaModule {
    private:
        static map<QualifiedName*, CajetaModule*> archive;
        string moduleName;
        string sourcePath;
        string archiveTargetPath;
        QualifiedName* qName;
        llvm::Module* module;
        CajetaModule(string moduleName, string sourceRoot, string sourcePath, string targetRoot) {
            this->moduleName = moduleName;
            this->
        }
    public:
        static CajetaModule* create(string typeName, string package) {
            QualifiedName* qName = QualifiedName::create()
        }
    };
}

