//
// Created by James Klappenbach on 4/19/23.
//

#include "NewExpression.h"
#include "cajeta/compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* NewExpression::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
//        FieldPtr field = module->getFieldStack().back();
//        CajetaTypePtr type = field->getType();
//        llvm::Constant* allocSize = llvm::ConstantExpr::getSizeOf(type->getLlvmType());
//        llvm::Instruction* mallocInst = MemoryManager::createMallocInstruction(module, allocSize,
//            module->getBuilder()->GetInsertBlock());
//        module->setCurrentValue(mallocInst);
//        this->creatorRest->generateCode(module);
//        return mallocInst;
        module->getAsnStack().pop_back();

        return nullptr;
    }
} // cajeta