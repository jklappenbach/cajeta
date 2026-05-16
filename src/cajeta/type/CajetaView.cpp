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
        // Layout strategy:
        //   - The LLVM struct holds all FIXED-SIZE fields in declaration order.
        //   - Variable-size fields (`String`, `T[]`) are NOT in the LLVM struct.
        //     Their inline `i32 length + data` payload lives past the struct's
        //     footprint in the buffer. Field accessors compute their offsets
        //     at runtime via a walk-the-prefixes scheme — for the Kth variable-
        //     size field, walk K-1 prior length-prefixes to find the offset.
        //   - All variable-size fields must be trailing. Fixed fields after a
        //     variable-size field need a runtime offset cache (deferred to a
        //     follow-up session — see StructsViewsStatus.md S5b notes).
        //
        // The shift from S4's "first var-size prefix lives in the struct" to
        // S5's "no var-size in the struct" simplifies the multi-trailing case:
        // every var-size accessor walks from the same starting point
        // (fixedPrefixSize), and the LLVM struct represents purely the fixed
        // prefix — no special-casing the first var-size field's slot index.
        vector<llvm::Type*> llvmMembers;
        llvmMembers.reserve(propertyList.size());
        bool sawVariableSize = false;
        int variableSizeCount = 0;
        for (auto& property : propertyList) {
            // Direct recursion guard: a view containing itself is infinite
            // size. Layout-pass cycle detection in v1 only catches the
            // direct case; deeper transitive cycles (A contains B, B
            // contains A) need a fuller graph walk that's deferred until
            // views compose into more complex shapes.
            auto fieldType = property->getType();
            if (fieldType && fieldType->getQName()
                    && fieldType->getQName()->toCanonical() == canonical) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "view '%s' has field '%s' of its own type; recursive views are "
                    "forbidden (would have infinite size). Use a class reference if "
                    "you need recursive structure.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_VIEW_RECURSIVE");
            }
            bool isVar = CajetaAggregate::isVariableSize(property);
            if (sawVariableSize && !isVar) {
                // Fixed field after a variable-size field needs the runtime
                // offset cache machinery — that lands in S5b. Until then we
                // reject so users get a clear error instead of silent
                // miscompilation. Trailing variable-size fields ARE allowed
                // (each new var-size field appends to the trailing region).
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "view '%s' has a fixed-size field '%s' after a variable-size field; "
                    "post-variable fixed fields need an offset cache, deferred to a "
                    "follow-up session. Reorder so all variable-size fields are last.",
                    canonical.c_str(), property->getName().c_str());
                throw Exception(buf, "CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST");
            }
            if (isVar) {
                sawVariableSize = true;
                variableSizeCount += 1;
                // Variable-size field's bytes (i32 prefix + data) live past
                // the LLVM struct's footprint. Nothing pushed onto llvmMembers.
            } else {
                llvmMembers.push_back(property->getType()->getLlvmType());
            }
        }
        variableSizeFieldCount = variableSizeCount;
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

    uint64_t CajetaView::getMinimumSize() const {
        // Fixed-prefix bytes + 4 bytes per variable-size field's i32
        // length prefix. Minimum data per variable-size field is zero, so
        // this is the smallest buffer that can possibly back a valid view.
        return getFixedSize() + (uint64_t) variableSizeFieldCount * 4;
    }

} // namespace cajeta
