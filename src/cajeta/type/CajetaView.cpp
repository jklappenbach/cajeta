//
// CajetaView — see header for the design. Owns the byte-overlay codegen
// (variable-size / endianness / alignment) that's specific to views and
// doesn't apply to plain classes.
//

#include "CajetaView.h"
#include "CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../compile/CompilationContext.h"
#include "../method/Method.h"
#include "../error/Exception.h"

namespace cajeta {

    bool CajetaView::isVariableSize(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto type = property->getType();
        // String: stored inline as i32 length + UTF-8 bytes.
        auto qn = type->getQName();
        if (qn && qn->getTypeName() == "String") return true;
        // T[] where T is fixed-size: stored inline as i32 length +
        // length * sizeof(T) bytes (S5b). Variable-size element types
        // (T = String, T = nested-var-size-view) are not supported in v1
        // — the var-size codegen path doesn't handle the nested
        // length-prefix walk for them.
        if (dynamic_pointer_cast<CajetaArray>(type)) return true;
        return false;
    }

    llvm::Type* CajetaView::getLlvmType() {
        if (isFrozen() && CajetaType::rawLlvmType() == nullptr) {
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx && module) ctx = module->getLlvmContext();
            if (ctx) {
                // Re-create the fixed-prefix struct in this thread's context
                // (no registry side-effect) and refill its body. Mirrors the
                // member-selection in generatePrototype: fixed fields before the
                // first variable-size field; var-size + post-var fields live
                // past the struct footprint and are not members.
                std::string canonical = qName->toCanonical();
                llvm::StructType* st = CajetaType::getOrCreateLlvmStructNoRegister(ctx, canonical);
                setLlvmType(st);  // U6.4.2: bind before setBody so refs resolve
                std::vector<llvm::Type*> members;
                members.reserve(propertyList.size());
                bool sawVariableSize = false;
                for (auto& property : propertyList) {
                    if (CajetaView::isVariableSize(property)) { sawVariableSize = true; continue; }
                    if (sawVariableSize) continue;
                    members.push_back(property->getType()->getLlvmType());
                }
                const bool packed = (alignment != ViewAlignment::Natural);
                st->setBody(llvm::ArrayRef<llvm::Type*>(members), packed);
            }
        }
        return CajetaClass::getLlvmType();
    }

    void CajetaView::generatePrototype() {
        string canonical = qName->toCanonical();

        // Views.md § Endianness: a view declaration with no @BigEndian /
        // @LittleEndian / @HostEndian annotation defaults to host order
        // (`endianness` already initializes to ViewEndianness::Host, and the
        // Host path emits no bswap). `endiannessExplicit` stays false here so
        // a nested view with no annotation can still inherit its outer's order
        // (Views.md § Endianness inheritance) rather than forcing host.

        // Create the LLVM struct type. `getOrCreateLlvmType` also stuffs a
        // plain CajetaType into the canonical map; we'll overwrite that
        // immediately below so name lookups return this CajetaView instance.
        setLlvmType(CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical));  // U6.2
        typeMap[TypeKey(rawLlvmType())] = shared_from_this();
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
        //     follow-up session — see docs/history/StructsViewsStatus.md S5b notes).
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
            bool isVar = CajetaView::isVariableSize(property);
            if (isVar) {
                sawVariableSize = true;
                variableSizeCount += 1;
                // Variable-size field's bytes (i32 prefix + data) live past
                // the LLVM struct's footprint. Nothing pushed onto llvmMembers.
            } else if (sawVariableSize) {
                // Fixed field AFTER a variable-size field. The LLVM struct's
                // member list only contains the pre-first-var-size fixed
                // fields (those have compile-time-constant offsets). Post-
                // variable fixed fields aren't represented in the LLVM struct
                // at all — DotExpression's accessor walks all preceding
                // var-size length-prefixes at runtime to find them.
                // S5b drops the "fixed field after var-size is an error"
                // rejection that was in place during S5.
            } else {
                llvmMembers.push_back(property->getType()->getLlvmType());
            }
        }
        variableSizeFieldCount = variableSizeCount;
        const bool packed = (alignment != ViewAlignment::Natural);
        ((llvm::StructType*) rawLlvmType())->setBody(
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
        llvm::Type* lt = rawLlvmType();
        if (!lt || !llvm::isa<llvm::StructType>(lt)) {
            return 0;
        }
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getTypeAllocSize(lt);
    }

    uint64_t CajetaView::getMinimumSize() const {
        // Fixed-prefix bytes + 4 bytes per variable-size field's i32
        // length prefix. Minimum data per variable-size field is zero, so
        // this is the smallest buffer that can possibly back a valid view.
        return getFixedSize() + (uint64_t) variableSizeFieldCount * 4;
    }

} // namespace cajeta
