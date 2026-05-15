//
// CajetaStruct — POD aggregate with declared layout (Session 4 of the
// memory-model rollout). See `Structs.md` for the full doctrine.
//
// v1 covers: packed layout (host endian, fixed-size primitive fields).
// Variable-size fields (`String`, `T[]` inline), endianness intrinsics,
// `@Align(natural)`, and nested-struct layout land in Session 5.
//

#include "CajetaStruct.h"
#include "../compile/CajetaModule.h"
#include "../method/Method.h"
#include "../error/Exception.h"

namespace cajeta {

    bool CajetaStruct::isVariableSize(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto qn = property->getType()->getQName();
        if (qn && qn->getTypeName() == "String") return true;
        // CajetaArray-typed fields are variable-size too; handled in a later
        // pass when nested arrays-in-structs land.
        return false;
    }

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

        // Packed by default; @Align(natural) opts into LLVM's natural
        // alignment (inserts implicit padding between fields). Endianness is
        // a per-access concern (bswap on load/store) and doesn't affect the
        // layout itself — same byte offsets regardless.
        //
        // Variable-size fields (String today) substitute their i32 length
        // prefix into the LLVM struct. The data bytes live past the LLVM
        // struct in the buffer and are accessed via specialized DotExpression
        // codegen (see DotExpression.cpp). v1 restriction: at most one
        // variable-size field, and it must be the last field — fields after
        // a variable-size one would need runtime-computed offsets, deferred.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(*module->getLlvmContext());
        bool sawVariableSize = false;
        for (auto& property : propertyList) {
            if (sawVariableSize) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "struct '%s' has a fixed-size field '%s' after a variable-size field; "
                    "variable-size fields must be last (v1 restriction)",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST");
            }
            if (isVariableSize(property)) {
                // Substitute the length prefix (i32). The data bytes that
                // follow aren't part of the LLVM struct's footprint.
                llvmMembers.push_back(i32Ty);
                sawVariableSize = true;
            } else {
                llvmMembers.push_back(property->getType()->getLlvmType());
            }
        }
        const bool packed = (alignment != StructAlignment::Natural);
        ((llvm::StructType*) llvmType)->setBody(
            llvm::ArrayRef<llvm::Type*>(llvmMembers), packed);

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
