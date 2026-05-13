//
// Created by James Klappenbach on 11/4/22.
//

#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../type/CajetaArray.h"

namespace cajeta {
    llvm::Value* VariableDeclarator::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* VariableInitializer::generateCode(CajetaModulePtr module) {
        return children.back()->generateCode(module);
    }

    llvm::Value* ArrayInitializer::generateCode(CajetaModulePtr module) {
        // `int32[] xs = {1, 2, 3}` — allocate an array header of length N and
        // populate each slot in source order. The caller (LocalVariableDeclaration
        // or VariableInitializer) is responsible for calling setElementType before
        // codegen; without it we can't size the allocation or coerce values.
        if (!elementType) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();

        // Build the array type once so we can size header + element slots.
        auto arrayType = make_shared<CajetaArray>(module, elementType);
        module->getStructures()[arrayType->toCanonical()] =
            static_pointer_cast<CajetaClass>(arrayType);

        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_new_array_header");
        if (!allocFn) return nullptr;

        llvm::Type* headerTy = arrayType->getLlvmType();
        llvm::Type* elemTy = arrayType->getElementLlvmType(&ctx);
        uint64_t headerBytes = dl.getTypeAllocSize(headerTy);
        uint64_t elemBytes = dl.getTypeAllocSize(elemTy);

        int64_t count = (int64_t) children.size();
        llvm::Value* hdrPtr = builder->CreateCall(allocFn, {
            llvm::ConstantInt::get(i64Ty, headerBytes),
            llvm::ConstantInt::get(i64Ty, elemBytes),
            llvm::ConstantInt::get(i64Ty, count),
        });

        // Write each literal value into its data slot. GEP path:
        // pointer → struct → data array → element[idx]. Same layout the
        // runtime helpers and ArrayCreatorRest use.
        int idx = 0;
        for (auto& node : children) {
            llvm::Value* v = node->generateCode(module);
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                v = builder->CreateLoad(a->getAllocatedType(), a);
            }
            if (!v) { ++idx; continue; }
            // Numeric coercion to match the element slot's width — `{1, 2, 3}`
            // produces i32 literals, but the slot might be i8 or i64 depending
            // on element type. Mirrors what assignment-side coercion does.
            if (v->getType() != elemTy && elemTy->isIntegerTy() && v->getType()->isIntegerTy()) {
                v = builder->CreateIntCast(v, elemTy, /*isSigned=*/true);
            }
            vector<llvm::Value*> gepIndices = {
                llvm::ConstantInt::get(i64Ty, 0),
                llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                llvm::ConstantInt::get(i64Ty, idx),
            };
            llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
            builder->CreateStore(v, slot);
            ++idx;
        }

        return hdrPtr;
    }

} // code