//
// Created by James Klappenbach on 10/5/22.
//

#include "cajeta/asn/BlockStatement.h"
#include "cajeta/asn/Statement.h"


namespace cajeta {
    llvm::Value* DefaultBlockStatement::generateCode(CajetaModule* module) {
        return (*children.begin())->generateCode(module);
    }
}