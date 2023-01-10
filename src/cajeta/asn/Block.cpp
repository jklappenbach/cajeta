//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* Block::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());

        for (auto child: children) {
            child->generateCode(module);
        }
        module->getAsnStack().pop_back();
        return nullptr;
    }
}
