#include "SynthesizedSetterMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    SynthesizedSetterMethod::SynthesizedSetterMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            StructurePropertyPtr field)
        : Method(module, field->getName(), CajetaType::of("void"), parent),
          field(field) {
        this->parent = parent;
        // Parameter setup deferred to initParameter() — we need the
        // shared_ptr<SynthesizedSetterMethod> to exist before we can
        // call shared_from_this() to wire FormalParameter::setParent.
    }

    void SynthesizedSetterMethod::initParameter() {
        if (!parameterList.empty()) return;  // idempotent
        auto valueParam = make_shared<FormalParameter>(
            field->getName(), field->getType());
        valueParam->setParent(shared_from_this());
        parameterList.push_back(valueParam);
        parameters[valueParam->getName()] = valueParam;
    }

    void SynthesizedSetterMethod::generateCode() {
        // Method::generatePrototype built llvmFunction with signature
        // (this, value) -> void. arg(0) is this, arg(1) is the value.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        llvm::Value* value   = llvmFunction->getArg(1);

        int idx = parent->getFieldLlvmIndex(field);
        if (idx < 0) {
            throw Exception(
                "@Setter synthesizer: field '" + field->getName()
                + "' has no LLVM index on '"
                + parent->getQName()->toCanonical() + "'",
                "CAJETA_ERROR_SETTER_FIELD_INDEX");
        }

        llvm::Value* fieldPtr = b.CreateStructGEP(
            parent->getLlvmType(), thisPtr, (unsigned) idx,
            std::string("set.") + field->getName());

        // Store at the field's STORAGE shape. The incoming `value` arg
        // already has the right LLVM type per Method's signature
        // (class refs → ptr, primitives → native width, views/interfaces
        // → inline struct). Just store it.
        b.CreateStore(value, fieldPtr);
        b.CreateRetVoid();
    }
}
