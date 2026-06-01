//
// CP7-1a tests for the memory-facets classification core (FR-1.1, FR-1.2,
// FR-1.4). This is the pure vocabulary the debug-info codegen path uses to
// label each local/parameter with an allocation class and an ownership role.
// It deliberately takes a plain `FieldFacetInputs` (booleans gathered at the
// emitDbgLocal call site) rather than a CajetaModule/Field, so the
// classification logic unit-tests with no LLVM/compiler fixture (FR-8.4).
//
// The runtime ABI + host accessors that *carry* these facets are CP7-1b; the
// DAP wire is CP7-1d. This file pins down only the derivation rules.
//
#include <gtest/gtest.h>

#include <string>

#include "cajeta/dbg/MemoryFacets.h"

using cajeta::dbg::AllocClass;
using cajeta::dbg::OwnershipRole;
using cajeta::dbg::MemoryFacets;
using cajeta::dbg::FieldFacetInputs;
using cajeta::dbg::classifyField;
using cajeta::dbg::deriveAllocClass;
using cajeta::dbg::deriveOwnershipRole;
using cajeta::dbg::allocClassName;
using cajeta::dbg::ownershipRoleName;
using cajeta::dbg::LifetimeState;
using cajeta::dbg::LifetimeInputs;
using cajeta::dbg::deriveLifetime;
using cajeta::dbg::lifetimeStateName;

// ---- allocation class (FR-1.1) ----

TEST(MemoryFacetsAlloc, StackFieldIsStack) {
    FieldFacetInputs in;
    in.isStackField = true;
    EXPECT_EQ(deriveAllocClass(in), AllocClass::Stack);
}

TEST(MemoryFacetsAlloc, HeapFieldIsHeap) {
    FieldFacetInputs in;
    in.isHeapField = true;
    EXPECT_EQ(deriveAllocClass(in), AllocClass::Heap);
}

TEST(MemoryFacetsAlloc, SharedWinsOverFieldKind) {
    // The XPU `shared` placement is its own allocation class and takes
    // precedence over the StackField/HeapField slot choice.
    FieldFacetInputs in;
    in.isHeapField = true;
    in.isShared = true;
    EXPECT_EQ(deriveAllocClass(in), AllocClass::Shared);
}

TEST(MemoryFacetsAlloc, NeitherIsUnknownNeverDefaultedToStack) {
    // FR-1.1: not statically determinable => unknown, NEVER silently stack.
    FieldFacetInputs in;
    EXPECT_EQ(deriveAllocClass(in), AllocClass::Unknown);
}

// ---- ownership role (FR-1.2) ----

TEST(MemoryFacetsOwnership, DropEntryMakesOwner) {
    // A binding with a drop-chain entry owns a droppable value.
    FieldFacetInputs in;
    in.ownsDrop = true;
    EXPECT_EQ(deriveOwnershipRole(in), OwnershipRole::Owner);
}

TEST(MemoryFacetsOwnership, ReferenceWithoutDropIsBorrow) {
    FieldFacetInputs in;
    in.isReference = true;
    EXPECT_EQ(deriveOwnershipRole(in), OwnershipRole::Borrow);
}

TEST(MemoryFacetsOwnership, OwningViewIsOwnerNotBorrow) {
    // An owning struct view (`View(#buf)`) is a reference form that still owns
    // its backing buffer (drop entry present). Owner must win over borrow so
    // the owning view isn't mislabeled.
    FieldFacetInputs in;
    in.isReference = true;
    in.ownsDrop = true;
    EXPECT_EQ(deriveOwnershipRole(in), OwnershipRole::Owner);
}

TEST(MemoryFacetsOwnership, TransferredOutWinsOverEverything) {
    // A moved-out binding is consumed; that signal dominates so it is never
    // shown as a live owner/borrow (reading it is a language error, FR-4.3).
    FieldFacetInputs in;
    in.isReference = true;
    in.ownsDrop = true;
    in.transferredOut = true;
    EXPECT_EQ(deriveOwnershipRole(in), OwnershipRole::TransferredOut);
}

TEST(MemoryFacetsOwnership, PlainStackPrimitiveIsUnknownRole) {
    // `int32 x = 5`: not an owner (nothing to drop), not a borrow, not moved.
    // Ownership role is unknown/neutral; it renders without an ownership glyph.
    FieldFacetInputs in;
    in.isStackField = true;
    EXPECT_EQ(deriveOwnershipRole(in), OwnershipRole::Unknown);
}

// ---- combined classifyField + orthogonality (FR-4.6) ----

TEST(MemoryFacetsClassify, HeapOwnerComposes) {
    FieldFacetInputs in;
    in.isHeapField = true;
    in.ownsDrop = true;
    MemoryFacets f = classifyField(in);
    EXPECT_EQ(f.alloc, AllocClass::Heap);
    EXPECT_EQ(f.ownership, OwnershipRole::Owner);
}

TEST(MemoryFacetsClassify, HeapBorrowComposes) {
    FieldFacetInputs in;
    in.isHeapField = true;
    in.isReference = true;
    MemoryFacets f = classifyField(in);
    EXPECT_EQ(f.alloc, AllocClass::Heap);
    EXPECT_EQ(f.ownership, OwnershipRole::Borrow);
}

TEST(MemoryFacetsClassify, EmptyInputsAreFullyUnknown) {
    MemoryFacets f = classifyField(FieldFacetInputs{});
    EXPECT_EQ(f.alloc, AllocClass::Unknown);
    EXPECT_EQ(f.ownership, OwnershipRole::Unknown);
}

// ---- textual affordance (FR-3/FR-5.3: color is never the sole carrier) ----

TEST(MemoryFacetsNames, AllocClassNames) {
    EXPECT_STREQ(allocClassName(AllocClass::Stack), "stack");
    EXPECT_STREQ(allocClassName(AllocClass::Heap), "heap");
    EXPECT_STREQ(allocClassName(AllocClass::Shared), "shared");
    EXPECT_STREQ(allocClassName(AllocClass::Unknown), "unknown");
}

TEST(MemoryFacetsNames, OwnershipRoleNames) {
    EXPECT_STREQ(ownershipRoleName(OwnershipRole::Owner), "owner");
    EXPECT_STREQ(ownershipRoleName(OwnershipRole::Borrow), "borrow");
    EXPECT_STREQ(ownershipRoleName(OwnershipRole::TransferredOut), "moved");
    EXPECT_STREQ(ownershipRoleName(OwnershipRole::Unknown), "unknown");
}

// ---- lifetime derivation at a stop (CP7-1c, FR-2.2) ----

TEST(MemoryFacetsLifetime, ActiveOwnerIsAboutToDrop) {
    LifetimeInputs in;
    in.ownership = OwnershipRole::Owner;
    in.hasDropEntry = true;
    in.dropEntryActive = true;
    EXPECT_EQ(deriveLifetime(in), LifetimeState::AboutToDrop);
}

TEST(MemoryFacetsLifetime, InactiveOwnerIsMovedOut) {
    LifetimeInputs in;
    in.ownership = OwnershipRole::Owner;
    in.hasDropEntry = true;
    in.dropEntryActive = false;   // entry deactivated => moved out at runtime
    EXPECT_EQ(deriveLifetime(in), LifetimeState::MovedOut);
}

TEST(MemoryFacetsLifetime, TransferredOutRoleIsMovedOut) {
    LifetimeInputs in;
    in.ownership = OwnershipRole::TransferredOut;   // static move-out dominates
    EXPECT_EQ(deriveLifetime(in), LifetimeState::MovedOut);
}

TEST(MemoryFacetsLifetime, BorrowIsLive) {
    LifetimeInputs in;
    in.ownership = OwnershipRole::Borrow;
    EXPECT_EQ(deriveLifetime(in), LifetimeState::Live);
}

TEST(MemoryFacetsLifetime, PlainValueIsLive) {
    LifetimeInputs in;   // Unknown ownership, no drop entry: an in-scope value
    EXPECT_EQ(deriveLifetime(in), LifetimeState::Live);
}

TEST(MemoryFacetsLifetime, OwnerWithoutTrackedEntryIsLive) {
    LifetimeInputs in;
    in.ownership = OwnershipRole::Owner;   // anomalous: no entry -> not misleading
    EXPECT_EQ(deriveLifetime(in), LifetimeState::Live);
}

TEST(MemoryFacetsNames, LifetimeStateNames) {
    EXPECT_STREQ(lifetimeStateName(LifetimeState::Live), "live");
    EXPECT_STREQ(lifetimeStateName(LifetimeState::MovedOut), "moved-out");
    EXPECT_STREQ(lifetimeStateName(LifetimeState::AboutToDrop), "about-to-drop");
    EXPECT_STREQ(lifetimeStateName(LifetimeState::Unknown), "unknown");
}
