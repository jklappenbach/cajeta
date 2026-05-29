//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaArray.h"
#include "CajetaView.h"
#include "../compile/CajetaModule.h"

namespace cajeta {

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

    CajetaArray::CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType) : CajetaClass(module) {
        this->elementType = elementType;
        string typeName = elementType->toCanonical() + "[]";
        qName = QualifiedName::getOrCreate(typeName);
        canonical = qName->toCanonical();

        llvm::LLVMContext* ctx = module->getLlvmContext();
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
        llvmType = CajetaType::getOrCreateLlvmType(ctx, string("#array.") + canonical, fields);
    }
}
