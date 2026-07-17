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
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
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

        // Second: evaluate any per-field initializer (`int32 x = 5;`)
        // and store, overriding the zero-init from pass one. Runs
        // BEFORE arg writes so that ctor params still override
        // initializers when both apply — Java semantics: field
        // initializers run as part of the implicit-construction
        // sequence, then the ctor body (here, the synthesized arg
        // writes) executes. `module->getBuilder()` is what the
        // initializer's expression codegen consults, so swap the
        // module's builder onto our local IRBuilder for the
        // duration (mirrors SynthesizedBuilderFactoryMethod's
        // @Builder.Default loop).
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
                    // Twin of the user-ctor path in Method.cpp: the field HAS an
                    // initializer and it lowered to nothing, so `continue` left
                    // the field silently zero (silent-resolution diagnostics
                    // 3.1.3). A class with no user ctor inits its fields HERE.
                    throw locatedException(
                        init->getSourceLine(), init->getSourceColumn() + 1,
                        "initializer for field '" + prop->getName()
                            + "' did not resolve to a value",
                        "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
                }
                llvm::Value* fp = b.CreateStructGEP(
                    parent->getLlvmType(), thisPtr, (unsigned) idx,
                    std::string("ctor.init.") + prop->getName());
                // Coerce when the initializer's natural LLVM type
                // doesn't match the field slot's width. Mirrors
                // StackField.cpp's coercion logic and the @Builder.
                // Default coercion arm in SynthesizedBuilderMethods.
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

        // Third: store each param into its target field, in declaration
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
