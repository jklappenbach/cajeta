//
// CajetaStruct — POD aggregate with declared layout (Session 4 of the
// memory-model rollout). See `WireFormats.md` for the full doctrine.
//
// v1 covers: packed layout (host endian, fixed-size primitive fields).
// Variable-size fields (`String`, `T[]` inline), endianness intrinsics,
// `@Align(natural)`, and nested-struct layout land in Session 5.
//

#include "CajetaStruct.h"
#include "../compile/CajetaModule.h"
#include "../method/Method.h"

namespace cajeta {

    void CajetaStruct::generatePrototype() {
        string canonical = qName->toCanonical();

        // Create the LLVM struct type. `getOrCreateLlvmType` also stuffs a
        // plain CajetaType into the canonical map; we'll overwrite that
        // immediately below so name lookups return the struct's CajetaStruct
        // instance.
        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        // Also register by short name so the view constructor's name lookup
        // (`MyStruct(byte[])` in MethodCallExpression) finds the struct via
        // its bare typeName. This is single-file v1 — when multi-package
        // resolution lands, the lookup will use the importing scope instead.
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;

        // Packed layout — wire formats default to packed (no padding). The
        // `@Align(natural)` annotation that opts into natural alignment is a
        // Session 5 item; for now every struct is packed.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        for (auto& property : propertyList) {
            llvmMembers.push_back(property->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(
            llvm::ArrayRef<llvm::Type*>(llvmMembers),
            /*isPacked=*/true);

        // Structs are not `new`-able: no default constructor, no vtable. The
        // view constructor is synthesized on demand by MethodCallExpression's
        // intrinsic dispatch when it sees `MyStruct(byte[])`.

        // Register the struct with the module so MethodCallExpression's struct-
        // view dispatch can find it by name.
        CajetaModule::getStructureToModule()[canonical] = module;

        for (auto& methodEntry : methods) {
            methodEntry.second->generatePrototype();
        }
    }

    uint64_t CajetaStruct::getFixedSize() const {
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) {
            return 0;
        }
        // Sum primitive field sizes. Variable-size fields contribute their
        // length-prefix (i32 = 4 bytes) and are handled separately in Session 5.
        uint64_t total = 0;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        total = dl.getTypeAllocSize(llvmType);
        return total;
    }

} // namespace cajeta
