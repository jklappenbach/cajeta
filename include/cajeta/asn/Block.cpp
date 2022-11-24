//
// Created by James Klappenbach on 10/5/22.
//

#include <cajeta/asn/Block.h>
#include "cajeta/method/Method.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    llvm::Value* Block::generateCode(CajetaModule* module) {
        for (auto child : children) {
            llvm::Value* value = child->generateCode(module);
        }
        return nullptr;
    }
}
