//
// Created by James Klappenbach on 10/5/22.
//

#include "BlockStatement.h"
#include "Statement.h"
#include "../compile/CajetaModule.h"


namespace cajeta {
    llvm::Value* DefaultBlockStatement::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        llvm::Value* result = (*children.begin())->generateCode(module);
        module->getAsnStack().pop_back();
        return result;
    }
}