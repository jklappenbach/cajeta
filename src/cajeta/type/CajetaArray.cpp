//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaArray.h"
#include "CajetaView.h"
#include "../compile/CajetaModule.h"
#include "../compile/CompilationContext.h"

namespace cajeta {

    llvm::Type* CajetaArray::getLlvmType() {
        if (isFrozen() && CajetaType::rawLlvmType() == nullptr) {
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx && module) ctx = module->getLlvmContext();
            if (ctx) setLlvmType(buildLlvmType(ctx));
        }
        return CajetaClass::getLlvmType();
    }

    llvm::Type* CajetaArray::getElementLlvmType(llvm::LLVMContext* ctx) const {
        // Storing a reference element by value would break object identity and
        // recurse forever on a class with an array-of-itself field.
        if (dynamic_pointer_cast<CajetaArray>(elementType)) {
            return llvm::PointerType::get(*ctx, 0);
        }
        if (dynamic_pointer_cast<CajetaView>(elementType) == nullptr) {
            if (auto klass = dynamic_pointer_cast<CajetaClass>(elementType)) {
                // Key off the immutable wildcard identity, never STRUCT_FLAG or the
                // cached type: under stdlib reuse those drift to the concrete struct.
                if (klass->isWildcardInstantiation()) {
                    return llvm::PointerType::get(*ctx, 0);
                }
                CajetaTypeFlags flags = klass->getTypeFlags();
                if (!klass->isInterface()
                        && (flags & STRUCT_FLAG) == 0
                        && (flags & PRIMITIVE_FLAG) == 0) {
                    return llvm::PointerType::get(*ctx, 0);
                }
            }
        }
        return elementType->getLlvmType();
    }

    uint64_t CajetaArray::elementStrideBytes(const llvm::DataLayout& dl,
                                             llvm::LLVMContext* ctx) {
        llvm::Type* t = getLlvmType();
        if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(t)) {
            if (st->getNumElements() >= 2 && !st->isOpaque()) {
                if (auto* at = llvm::dyn_cast<llvm::ArrayType>(
                        st->getElementType(1))) {
                    return dl.getTypeAllocSize(at->getElementType());
                }
            }
        }
        return dl.getTypeAllocSize(getElementLlvmType(ctx));
    }

    llvm::Type* CajetaArray::getInlineLlvmType(llvm::LLVMContext* ctx) const {
        return llvm::ArrayType::get(getElementLlvmType(ctx),
                                    fixedLength >= 0 ? (uint64_t) fixedLength : 0);
    }

    CajetaArray::CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType,
                             int32_t fixedLength) : CajetaClass(module) {
        this->elementType = elementType;
        this->fixedLength = fixedLength;
        // The fixed length is in the canonical name, so `int8[64]` and `int8[]` differ.
        string typeName = elementType->toCanonical()
            + (fixedLength >= 0 ? ("[" + std::to_string(fixedLength) + "]") : "[]");
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        setLlvmType(buildLlvmType(module->getLlvmContext()));
    }

    llvm::Type* CajetaArray::buildLlvmType(llvm::LLVMContext* ctx) const {
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(*ctx);
        llvm::Type* elemLlvm = getElementLlvmType(ctx);

        // An element struct still unmaterialized is null or opaque, and unsized
        // types segfault ArrayType::get; reference elements are pointers anyway.
        if (elemLlvm == nullptr ||
            (elemLlvm->isStructTy() &&
             llvm::cast<llvm::StructType>(elemLlvm)->isOpaque())) {
            elemLlvm = llvm::PointerType::get(*ctx, 0);
        }

        // Header layout { i64 size, [0 x T] data }: the zero-length trailing array
        // is flexible data, and the allocation adds `count * sizeof(T)` bytes for it.
        vector<llvm::Type*> fields = {
            i64Ty,
            llvm::ArrayType::get(elemLlvm, 0),
        };
        return CajetaType::getOrCreateLlvmType(ctx, string("#array.") + canonical, fields);
    }

    llvm::Value* CajetaArray::emitElementBitsBase(llvm::IRBuilder<>& builder,
            llvm::Value* hdrPtr, uint64_t headerBytes, uint64_t elemBytes) {
        llvm::LLVMContext& ctx = builder.getContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Value* count = builder.CreateLoad(i64Ty, hdrPtr, "arr_count");
        llvm::Value* dataBytes = builder.CreateMul(count,
            llvm::ConstantInt::get(i64Ty, elemBytes));
        llvm::Value* offset = builder.CreateAdd(
            llvm::ConstantInt::get(i64Ty, headerBytes), dataBytes);
        return builder.CreateGEP(llvm::Type::getInt8Ty(ctx), hdrPtr, offset,
            "elem_bits");
    }
}
