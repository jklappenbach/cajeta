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
            if (ctx) setLlvmType(buildLlvmType(ctx));  // U6.4.2: per-thread rebuild
        }
        return CajetaClass::getLlvmType();
    }

    llvm::Type* CajetaArray::getElementLlvmType(llvm::LLVMContext* ctx) const {
        // Reference types are stored as opaque pointers in the array's
        // flexible-data slot:
        //  - Array elements: each inner array is its own heap allocation.
        //  - Reference-class instances: they are heap references. Storing
        //    the element struct by value would break the reference
        //    semantics array consumers rely on (object identity / sharing)
        //    AND recurse infinitely for a class with an array-of-itself
        //    field (e.g. BPlusTreeNode), whose element struct is incomplete
        //    during the class's own instantiation. A pointer is sized
        //    regardless — the same reason self-referential pointer fields
        //    (next / parent) already work.
        // Value structs, primitives, interfaces (fat pointers) and views
        // keep their own layout (returned by getLlvmType()).
        if (dynamic_pointer_cast<CajetaArray>(elementType)) {
            return llvm::PointerType::get(*ctx, 0);
        }
        if (dynamic_pointer_cast<CajetaView>(elementType) == nullptr) {
            if (auto klass = dynamic_pointer_cast<CajetaClass>(elementType)) {
                // A wildcard instantiation (e.g. Class<?>) is a reference proxy
                // that always flows by pointer — mirror CajetaClass::getLlvmType.
                // Key off the immutable wildcard identity, not STRUCT_FLAG / the
                // cached rawLlvmType: under stdlib reuse those drift to the
                // concrete {vtable,rtti} struct after accumulation, collapsing
                // Class<?>[] to 16-byte inline stride (the classesInPackage SIGSEGV).
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

    llvm::Type* CajetaArray::getInlineLlvmType(llvm::LLVMContext* ctx) const {
        return llvm::ArrayType::get(getElementLlvmType(ctx),
                                    fixedLength >= 0 ? (uint64_t) fixedLength : 0);
    }

    CajetaArray::CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType,
                             int32_t fixedLength) : CajetaClass(module) {
        this->elementType = elementType;
        this->fixedLength = fixedLength;
        // The canonical name encodes a fixed length so `int8[64]` (inline) and
        // `int8[]` (heap reference) are distinct types in the structures map.
        string typeName = elementType->toCanonical()
            + (fixedLength >= 0 ? ("[" + std::to_string(fixedLength) + "]") : "[]");
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        setLlvmType(buildLlvmType(module->getLlvmContext()));  // U6.4.1
    }

    llvm::Type* CajetaArray::buildLlvmType(llvm::LLVMContext* ctx) const {
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(*ctx);
        llvm::Type* elemLlvm = getElementLlvmType(ctx);

        // A reference-class element whose struct body isn't materialized
        // yet — e.g. a class with an array-of-itself field, resolved while
        // that very class is mid-instantiation (BPlusTreeNode) — yields a
        // null or opaque (unsized) type that llvm::ArrayType::get can't
        // accept (it would segfault). Reference elements are stored as
        // pointers regardless, so fall back to an opaque pointer.
        if (elemLlvm == nullptr ||
            (elemLlvm->isStructTy() &&
             llvm::cast<llvm::StructType>(elemLlvm)->isOpaque())) {
            elemLlvm = llvm::PointerType::get(*ctx, 0);
        }

        // Header layout: { i64 size, [0 x T] data }. The trailing zero-length array
        // is LLVM's way of expressing flexible data after the header; the actual
        // backing allocation is sized to include `count * sizeof(T)` bytes for the
        // data region.
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
