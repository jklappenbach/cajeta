//
// Created by James Klappenbach on 10/2/22.
//

#include "CajetaArray.h"
#include "../compile/CajetaModule.h"

namespace cajeta {

    llvm::Type* CajetaArray::getElementLlvmType(llvm::LLVMContext* ctx) const {
        // Array-of-array nests via a pointer at each level (each inner array is its
        // own heap allocation), so when the element type is itself an array store an
        // opaque pointer in the flexible-data slot.
        if (dynamic_pointer_cast<CajetaArray>(elementType)) {
            return llvm::PointerType::get(*ctx, 0);
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
