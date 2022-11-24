//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/method/DefaultDestructorMethod.h"
#include "cajeta/type/CajetaStructure.h"
#include "cajeta/asn/DefaultBlock.h"
#include "cajeta/compile/CajetaModule.h"

using namespace std;

namespace cajeta {
    /**
     * Default constructor method
     * @param name
     * @param returnType
     * @param parent
     */
    DefaultDestructorMethod::DefaultDestructorMethod(CajetaStructure* parent) :
            Method(string("~") + parent->getQName()->getTypeName(), CajetaType::of("void"), parent) {
        this->parent = parent;
        this->name = name;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        block = new DefaultBlock();
        scopes.push_back(parent->getScope());
    }

    void DefaultDestructorMethod::generateCode(CajetaModule* module) {
        llvmBasicBlock = llvm::BasicBlock::Create(*module->getLlvmContext(), name, llvmFunction);
        builder = new llvm::IRBuilder<>(llvmBasicBlock, llvmBasicBlock->begin());
        builder->SetInsertPoint(llvmBasicBlock);
        module->setBuilder(builder);
        module->setCurrentMethod(this);

        createScope(module);

        block->generateCode(module);
        destroyScope();
        module->getBuilder()->CreateRetVoid();
    }
}