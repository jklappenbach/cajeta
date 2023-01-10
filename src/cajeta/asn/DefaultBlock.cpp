//
// Created by James Klappenbach on 10/5/22.
//

#include "DefaultBlock.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* DefaultBlock::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }
}
