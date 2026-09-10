#include "../error/Diagnostics.h"
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
        // Deferred: shared_from_this() needs the owning shared_ptr to exist.
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
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // Signature: (this, field1, field2, ...) -> void, so arg(0) is `this`
        // and arg(i+1) carries fields[i]'s value.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Value* thisPtr = llvmFunction->getArg(0);

        // Pass 1: zero every non-static field, including those outside `fields`.
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

        // Pass 2: per-field initializers, BEFORE the arg writes so a ctor param
        // still wins when both apply. Initializer codegen reads the module's
        // builder, so swap ours in for the duration.
        {
            auto* prevBuilder = module->getBuilder();
            module->setBuilder(&b);
            for (auto& prop : parent->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                auto init = prop->getInitializer();
                if (!init) continue;
                int idx = parent->getFieldLlvmIndex(prop);
                if (idx < 0) continue;
                llvm::Value* initVal = init->generateCode(module);
                if (!initVal) {
                    // The field HAS an initializer that lowered to nothing:
                    // continuing would leave it silently zero.
                    throw locatedException(
                        init->getSourceLine(), init->getSourceColumn() + 1,
                        "initializer for field '" + prop->getName()
                            + "' did not resolve to a value",
                        "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
                }
                llvm::Value* fp = b.CreateStructGEP(
                    parent->getLlvmType(), thisPtr, (unsigned) idx,
                    std::string("ctor.init.") + prop->getName());
                CajetaTypePtr ft = prop->getType();
                llvm::Type* slotTy = ft ? ft->getLlvmType() : nullptr;
                if (slotTy && initVal->getType() != slotTy) {
                    llvm::Type* srcTy = initVal->getType();
                    if (slotTy->isIntegerTy() && srcTy->isIntegerTy()) {
                        initVal = b.CreateIntCast(initVal, slotTy, /*isSigned=*/true);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isFloatingPointTy()) {
                        initVal = b.CreateFPCast(initVal, slotTy);
                    } else if (slotTy->isFloatingPointTy() && srcTy->isIntegerTy()) {
                        initVal = b.CreateSIToFP(initVal, slotTy);
                    } else if (slotTy->isIntegerTy() && srcTy->isFloatingPointTy()) {
                        initVal = b.CreateFPToSI(initVal, slotTy);
                    }
                }
                b.CreateStore(initVal, fp);
            }
            module->setBuilder(prevBuilder);
        }

        // Pass 3: store each param into its field; arg i+1 is fields[i].
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
