#include "SynthesizedConstructorMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    SynthesizedConstructorMethod::SynthesizedConstructorMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            std::vector<StructurePropertyPtr> fields)
        : Method(module, parent->getQName()->getTypeName(),
                 CajetaType::of("void"), parent),
          fields(std::move(fields)) {
        this->parent = parent;
        // FormalParameter setup deferred to initParameters() so we can
        // call shared_from_this() to wire each param's parent.
    }

    void SynthesizedConstructorMethod::initParameters() {
        if (!parameterList.empty()) return;  // idempotent
        for (auto& f : fields) {
            auto p = make_shared<FormalParameter>(f->getName(), f->getType());
            p->setParent(shared_from_this());
            parameterList.push_back(p);
            parameters[p->getName()] = p;
        }
    }

    void SynthesizedConstructorMethod::generateCode() {
        // Signature post-prototype: (this, field1, field2, ...) -> void.
        // arg(0) is this; arg(i+1) is fields[i]'s value.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Value* thisPtr = llvmFunction->getArg(0);

        // First: zero-initialize EVERY non-static field so any field not
        // in `fields` (e.g. @NoArgsConstructor with no per-field defaults)
        // starts at a defined value. Primitives → 0; class refs / arrays
        // → null pointer; views / interfaces → undef-zeroed struct.
        for (auto& prop : parent->getPropertyList()) {
            if (!prop || prop->isStatic()) continue;

            int idx = parent->getFieldLlvmIndex(prop);
            if (idx < 0) continue;
            llvm::Value* fp = b.CreateStructGEP(
                parent->getLlvmType(), thisPtr, (unsigned) idx,
                std::string("ctor.zero.") + prop->getName());

            CajetaTypePtr ft = prop->getType();
            llvm::Type* slotTy;
            bool slotIsPtr = false;
            if (dynamic_pointer_cast<CajetaArray>(ft)) {
                slotTy = llvm::PointerType::get(ctx, 0);
                slotIsPtr = true;
            } else if (auto cls = dynamic_pointer_cast<CajetaClass>(ft)) {
                if (dynamic_pointer_cast<CajetaView>(ft)) {
                    slotTy = ft->getLlvmType();
                } else if (cls->isInterface()) {
                    slotTy = ft->getLlvmType();
                } else {
                    slotTy = llvm::PointerType::get(ctx, 0);
                    slotIsPtr = true;
                }
            } else {
                slotTy = ft->getLlvmType();
            }

            llvm::Constant* zeroVal;
            if (slotIsPtr) {
                zeroVal = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(slotTy));
            } else {
                zeroVal = llvm::Constant::getNullValue(slotTy);
            }
            b.CreateStore(zeroVal, fp);
        }

        // Second: store each param into its target field, in declaration
        // order. Args at LLVM index i+1 correspond to fields[i].
        for (size_t i = 0; i < fields.size(); ++i) {
            auto& prop = fields[i];
            int idx = parent->getFieldLlvmIndex(prop);
            if (idx < 0) {
                throw Exception(
                    "@*Constructor synthesizer: field '" + prop->getName()
                    + "' has no LLVM index on '"
                    + parent->getQName()->toCanonical() + "'",
                    "CAJETA_ERROR_CTOR_FIELD_INDEX");
            }
            llvm::Value* fp = b.CreateStructGEP(
                parent->getLlvmType(), thisPtr, (unsigned) idx,
                std::string("ctor.f.") + prop->getName());
            llvm::Value* val = llvmFunction->getArg((unsigned) (i + 1));
            b.CreateStore(val, fp);
        }

        b.CreateRetVoid();
    }

}
