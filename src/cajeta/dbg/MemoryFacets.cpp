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

    LifetimeState deriveLifetime(const LifetimeInputs& in) {
        // Static move-out (the `#`-transferred-out role) dominates.
        if (in.ownership == OwnershipRole::TransferredOut)
            return LifetimeState::MovedOut;
        // An owner's runtime drop entry decides between still-scheduled-to-drop
        // and moved-out-at-runtime (the entry was deactivated on transfer).
        if (in.ownership == OwnershipRole::Owner && in.hasDropEntry)
            return in.dropEntryActive ? LifetimeState::AboutToDrop
                                      : LifetimeState::MovedOut;
        // Borrows, plain values, and owners without a tracked entry: a
        // registered local is in scope, so it is live.
        return LifetimeState::Live;
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

    const char* lifetimeStateName(LifetimeState s) {
        switch (s) {
            case LifetimeState::Live:        return "live";
            case LifetimeState::MovedOut:    return "moved-out";
            case LifetimeState::AboutToDrop: return "about-to-drop";
            case LifetimeState::Unknown:
            default:                         return "unknown";
        }
    }

} // namespace cajeta::dbg
