//
// Created by James Klappenbach on 10/5/22.
//

#include "BlockStatement.h"
#include "Statement.h"
#include "../compile/CajetaModule.h"


namespace cajeta {
    llvm::Value* DefaultBlockStatement::generateCode(CajetaModulePtr module) {
        llvm::Value* result = (*children.begin())->generateCode(module);
        return result;
    }
}