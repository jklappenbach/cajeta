// Memory-facets vocabulary for the debugger: every registered local carries an
// allocation class (where it lives) and an ownership role (who is responsible).
#pragma once

#include <cstdint>

namespace cajeta::dbg {

    // Where a binding's value lives. `Unknown` is a first-class state for the
    // not-statically-determinable case, never silently rendered as Stack.
    enum class AllocClass : uint8_t {
        Unknown = 0,
        Stack   = 1,  // frame-local inline value (StackField)
        Heap    = 2,  // new T(...) / class instance / array reference (HeapField)
        Shared  = 3,  // XPU `shared` placement (kernel-launch lifetime)
    };

    // Who is responsible for a binding's referent. FR-1.2.
    enum class OwnershipRole : uint8_t {
        Unknown        = 0,
        Owner          = 1,  // sole binding responsible for dropping the value
        Borrow         = 2,  // non-owning, aliasing reference
        TransferredOut = 3,  // ownership moved out via `#`; the binding is consumed
    };

    // The binding's lifetime state AT A STOP. Dynamic, unlike the other two facets:
    // the host derives it from drop-chain state while walking frames, not codegen.
    enum class LifetimeState : uint8_t {
        Unknown     = 0,
        Live        = 1,  // holds a valid value; in scope, not moved out
        MovedOut    = 2,  // ownership transferred away; reading it is an error
        AboutToDrop = 3,  // a live owner on the drop chain, scheduled to drop
    };

    struct MemoryFacets {
        AllocClass alloc = AllocClass::Unknown;
        OwnershipRole ownership = OwnershipRole::Unknown;
    };

    // Signals gathered at the emitDbgLocal call site. All default false, so an
    // undetermined facet degrades to Unknown rather than to a wrong concrete value.
    struct FieldFacetInputs {
        bool isStackField   = false;
        bool isHeapField    = false;
        bool isShared       = false;
        bool isReference    = false;
        bool ownsDrop       = false;
        bool transferredOut = false;
    };

    // Precedence: shared placement wins over the slot kind, then stack, then heap;
    // otherwise Unknown, never defaulted to Stack.
    AllocClass deriveAllocClass(const FieldFacetInputs& in);

    // Precedence: moved-out dominates, so it cannot present as a live owner or borrow;
    // then drop-owning is an Owner (an owning view is not a borrow); then a reference.
    OwnershipRole deriveOwnershipRole(const FieldFacetInputs& in);

    MemoryFacets classifyField(const FieldFacetInputs& in);

    // Signals gathered at a STOP. `hasDropEntry`/`dropEntryActive` come from the
    // runtime drop chain, whose `active` flag clears when ownership is moved out.
    struct LifetimeInputs {
        OwnershipRole ownership      = OwnershipRole::Unknown;
        bool          hasDropEntry   = false;
        bool          dropEntryActive = false;
    };

    // TransferredOut dominates; a live drop entry is AboutToDrop, a deactivated one
    // MovedOut, everything else Live. Never Unknown: that is the pre-derivation default.
    LifetimeState deriveLifetime(const LifetimeInputs& in);

    // Stable lowercase tags, carried over the wire so meaning never rests on color
    // alone; TransferredOut prints as the user-facing "moved".
    const char* allocClassName(AllocClass c);
    const char* ownershipRoleName(OwnershipRole r);
    const char* lifetimeStateName(LifetimeState s);

} // namespace cajeta::dbg
