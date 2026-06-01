//
// Debugger CP7-1a: memory-facets vocabulary for ownership / allocation /
// lifetime visualization (see ide-plugins/idea/ide-plugin-debug-fr-1.md,
// FR-1.1/FR-1.2/FR-1.4). Each named local/parameter the debug-info codegen
// path registers gets two orthogonal facets: an *allocation class* (where the
// value lives) and an *ownership role* (who is responsible for it).
//
// This is the pure classification core. It takes a plain `FieldFacetInputs`
// (booleans gathered at the emitDbgLocal call site from the StackField/
// HeapField choice, isReference, the drop-chain entry, and #-transfer) and
// yields the facets — no LLVM, no CajetaModule, no Field. Keeping it
// dependency-free means the derivation rules unit-test without a compiler
// fixture (FR-8.4), mirroring DebugVars' plain formatValue/writeValue core.
//
// The runtime ABI that *carries* these facets through the debug frame chain
// (extending __cajeta_dbg_local) and the host accessors are CP7-1b; the DAP
// `variables` wire is CP7-1d; rendering is CP7-2..4. This header is only the
// shared vocabulary + rules those layers agree on.
//
#pragma once

#include <cstdint>

namespace cajeta::dbg {

    // Where a binding's value lives. FR-1.1. `Unknown` is a first-class state
    // for the not-statically-determinable case — never silently treated as
    // Stack, so the UI can render it neutrally (FR-4.5) rather than miscolor it.
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

    // The binding's lifetime state AT A STOP (FR-1.2 / FR-2.2). Unlike alloc
    // class and ownership role (largely static), this is dynamic: it depends on
    // runtime drop-chain state at the parked safepoint, so it is derived by the
    // host when it walks frames, not baked in at codegen time. `Unknown` is the
    // wire default before derivation runs.
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
    // undetermined facet degrades to Unknown rather than a wrong concrete value
    // (FR-1.4). `isStackField`/`isHeapField` come from the StackField/HeapField
    // choice already made for the local's slot; `isShared` from the XPU `shared`
    // placement; `isReference` from the borrow form; `ownsDrop` from the field
    // carrying a drop-chain entry (the authoritative owner signal — set iff the
    // binding owns a droppable); `transferredOut` from `#`-transfer move tracking.
    struct FieldFacetInputs {
        bool isStackField   = false;
        bool isHeapField    = false;
        bool isShared       = false;
        bool isReference    = false;
        bool ownsDrop       = false;
        bool transferredOut = false;
    };

    // Allocation class. Precedence: shared placement wins over the slot kind;
    // then stack, then heap; otherwise unknown (never defaulted to stack).
    AllocClass deriveAllocClass(const FieldFacetInputs& in);

    // Ownership role. Precedence: a moved-out binding is consumed and dominates
    // (it must not present as a live owner/borrow, FR-4.3); then a drop-owning
    // binding is an Owner (so an owning view, which is also a reference, is not
    // mislabeled a borrow); then a reference is a Borrow; otherwise unknown.
    OwnershipRole deriveOwnershipRole(const FieldFacetInputs& in);

    // Both facets at once.
    MemoryFacets classifyField(const FieldFacetInputs& in);

    // Signals gathered at a STOP for lifetime derivation (CP7-1c). `ownership`
    // is the already-classified role; `hasDropEntry`/`dropEntryActive` come from
    // the runtime drop chain (an owner registers a `cajeta_drop_entry` whose
    // `active` flag is cleared when ownership is moved out). All-false degrades
    // to Live for an in-scope binding — never a misleading moved-out.
    struct LifetimeInputs {
        OwnershipRole ownership      = OwnershipRole::Unknown;
        bool          hasDropEntry   = false;
        bool          dropEntryActive = false;
    };

    // Lifetime state. A statically moved-out role (TransferredOut) dominates;
    // then an owner with a live drop entry is AboutToDrop, and one whose entry
    // has been deactivated (moved out at runtime) is MovedOut; everything else
    // in scope — borrows, plain values, owners without a tracked entry — is
    // Live. Never returns Unknown: a registered local is, by construction, in
    // scope (Unknown is reserved as the pre-derivation wire default).
    LifetimeState deriveLifetime(const LifetimeInputs& in);

    // Stable lowercase tags, also the textual affordance carried over the wire
    // so meaning never rests on color alone (FR-5.3). TransferredOut prints as
    // "moved" (the user-facing term).
    const char* allocClassName(AllocClass c);
    const char* ownershipRoleName(OwnershipRole r);
    const char* lifetimeStateName(LifetimeState s);

} // namespace cajeta::dbg
