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
        // A template instantiation's typeName carries the arg suffix
        // (`Stream<cajeta.int32>`), but constructor calls resolve by the
        // simple name (`Stream`) — the same name declared ctors get from the
        // re-parsed template source. Name the synthesized default the same
        // way or it is unresolvable on ctor-less instantiations.
        : Method(module,
              parent->getTemplateOrigin()
                  ? parent->getTemplateOrigin()->getQName()->getTypeName()
                  : parent->getQName()->getTypeName(),
              CajetaType::of("void"), parent) {
        this->parent = parent;
        constructor = true;
        block = make_shared<DefaultBlock>();
    }

    void DefaultConstructorMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
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