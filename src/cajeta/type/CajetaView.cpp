//
// CajetaView — see header for the design. Owns the legacy view-style codegen
// previously in CajetaStruct::generatePrototype, with the variable-size /
// endianness / alignment handling reorganized under the CajetaAggregate
// hierarchy.
//

#include "CajetaView.h"
#include "../compile/CajetaModule.h"
#include "../method/Method.h"
#include "../error/Exception.h"

namespace cajeta {

    void CajetaView::generatePrototype() {
        string canonical = qName->toCanonical();

        // Views.md § Endianness: every view declaration must carry one of
        // @BigEndian / @LittleEndian / @HostEndian. There is no silent
        // default — host-endian assumptions break when code moves between
        // architectures. Reject here so the error surfaces at type
        // registration, before any usage codegen runs.
        if (!endiannessExplicit) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "view '%s' is missing an endianness annotation; declare it "
                "with one of @BigEndian, @LittleEndian, or @HostEndian "
                "(see Views.md \xc2\xa7 Endianness)",
                canonical.c_str());
            throw Exception(buf, "CAJETA_ERROR_VIEW_ENDIANNESS_REQUIRED");
        }

        // Create the LLVM struct type. `getOrCreateLlvmType` also stuffs a
        // plain CajetaType into the canonical map; we'll overwrite that
        // immediately below so name lookups return this CajetaView instance.
        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        // Also register by short name so the view constructor's name lookup
        // (`MyView(byte[])` in MethodCallExpression) finds the view via its
        // bare typeName. Multi-package resolution lands later.
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
        // codegen. v1 restriction: at most one variable-size field, must be
        // last. S5 lifts both restrictions.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(*module->getLlvmContext());
        bool sawVariableSize = false;
        for (auto& property : propertyList) {
            if (sawVariableSize) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "view '%s' has a fixed-size field '%s' after a variable-size field; "
                    "variable-size fields must be last (v1 restriction)",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST");
            }
            if (CajetaAggregate::isVariableSize(property)) {
                llvmMembers.push_back(i32Ty);
                sawVariableSize = true;
            } else {
                llvmMembers.push_back(property->getType()->getLlvmType());
            }
        }
        const bool packed = (alignment != ViewAlignment::Natural);
        ((llvm::StructType*) llvmType)->setBody(
            llvm::ArrayRef<llvm::Type*>(llvmMembers), packed);

        // Views are not `new`-able: no default constructor, no vtable. The
        // view constructor is synthesized on demand by MethodCallExpression's
        // intrinsic dispatch when it sees `MyView(byte[])`.

        // Register with the module so MethodCallExpression's view-construction
        // dispatch can find it by canonical name.
        CajetaModule::getStructureToModule()[canonical] = module;

        for (auto& methodEntry : methods) {
            methodEntry.second->generatePrototype();
        }
    }

    uint64_t CajetaView::getFixedSize() const {
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) {
            return 0;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getTypeAllocSize(llvmType);
    }

} // namespace cajeta
