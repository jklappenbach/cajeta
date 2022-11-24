//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/method/ArrayDestructorMethod.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/asn/DefaultBlock.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/type/CajetaArray.h"

using namespace std;

namespace cajeta {
    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    ArrayDestructorMethod::ArrayDestructorMethod(CajetaStructure* parent) :
            Method(string("~") + parent->getQName()->getTypeName(), CajetaType::of("void"), parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = new DefaultBlock();
        scopes.push_back(parent->getScope());
    }

    void ArrayDestructorMethod::generateCode(CajetaModule* module) {
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(this);

        createScope(module);
        //block->generateCode(module);
        MemoryManager* memoryManager = MemoryManager::getInstance(module);
        Field* field = parent->getFields()[CajetaArray::ARRAY_FIELD_NAME];
        memoryManager->createFreeInstruction(field->getAllocation(), builder->GetInsertBlock());
        destroyScope();
        module->getBuilder()->CreateRetVoid();
    }
}