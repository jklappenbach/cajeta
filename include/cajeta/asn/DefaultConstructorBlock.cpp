//
// Created by James Klappenbach on 10/5/22.
//

#include <cajeta/type/Method.h>
#include <cajeta/asn/DefaultConstructorBlock.h>
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    llvm::Value* DefaultConstructorBlock::generateCode(CajetaModule* module) {
        module->getBuilder()->CreateRetVoid();
        return nullptr;
    }
}
