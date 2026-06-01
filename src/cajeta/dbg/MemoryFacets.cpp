//
// CP7-1a: memory-facets classification core. See MemoryFacets.h.
//
#include "cajeta/dbg/MemoryFacets.h"

namespace cajeta::dbg {

    AllocClass deriveAllocClass(const FieldFacetInputs& in) {
        if (in.isShared) return AllocClass::Shared;
        if (in.isStackField) return AllocClass::Stack;
        if (in.isHeapField) return AllocClass::Heap;
        return AllocClass::Unknown;
    }

    OwnershipRole deriveOwnershipRole(const FieldFacetInputs& in) {
        // Moved-out dominates: a consumed binding must never read as live.
        if (in.transferredOut) return OwnershipRole::TransferredOut;
        // Owns a droppable (incl. the owning-view form, which is also a
        // reference) => Owner, ahead of the borrow check.
        if (in.ownsDrop) return OwnershipRole::Owner;
        if (in.isReference) return OwnershipRole::Borrow;
        return OwnershipRole::Unknown;
    }

    MemoryFacets classifyField(const FieldFacetInputs& in) {
        return MemoryFacets{deriveAllocClass(in), deriveOwnershipRole(in)};
    }

    const char* allocClassName(AllocClass c) {
        switch (c) {
            case AllocClass::Stack:  return "stack";
            case AllocClass::Heap:   return "heap";
            case AllocClass::Shared: return "shared";
            case AllocClass::Unknown:
            default:                 return "unknown";
        }
    }

    const char* ownershipRoleName(OwnershipRole r) {
        switch (r) {
            case OwnershipRole::Owner:          return "owner";
            case OwnershipRole::Borrow:         return "borrow";
            case OwnershipRole::TransferredOut: return "moved";
            case OwnershipRole::Unknown:
            default:                            return "unknown";
        }
    }

} // namespace cajeta::dbg
