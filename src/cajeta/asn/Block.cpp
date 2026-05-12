//
// Created by James Klappenbach on 10/5/22.
//

#include "Block.h"
#include "../method/Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* Block::generateCode(CajetaModulePtr module) {

        for (auto child: children) {
            child->generateCode(module);
        }
        return nullptr;
    }
}
