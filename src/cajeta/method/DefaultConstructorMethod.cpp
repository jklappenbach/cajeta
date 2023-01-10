//
// Created by James Klappenbach on 2/19/22.
//

#include "DefaultConstructorMethod.h"
#include "../type/CajetaClass.h"
#include "../asn/DefaultBlock.h"
#include "../compile/CajetaModule.h"

using namespace std;

namespace cajeta {
    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    DefaultConstructorMethod::DefaultConstructorMethod(CajetaModulePtr module, CajetaClassPtr parent)
        : Method(module, parent->getQName()->getTypeName(), CajetaType::of("void"), parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = make_shared<DefaultBlock>();
    }

    void DefaultConstructorMethod::generateCode() {
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        createScope();
        block->generateCode(module);
        destroyScope();

        module->getBuilder()->CreateRetVoid();
    }
}