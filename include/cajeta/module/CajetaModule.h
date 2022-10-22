//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "llvm/IR/Module.h"
#include "cajeta/module/QualifiedName.h"

namespace cajeta {
    class CajetaModule {
    private:
        CajetaModule* theInstance;
        QualifiedName* gName;
        llvm::Module* module;
        CajetaModule() { }
    public:
        static CajetaModule* getInstance() {

        }
    };
}

