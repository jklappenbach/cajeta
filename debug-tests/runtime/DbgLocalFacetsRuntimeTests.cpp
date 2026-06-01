//
// CP7-1b tests for the memory-facet bytes carried through the debug frame
// chain. Exercises the NATIVE runtime copy linked into this binary: push a
// frame, register locals via the extended __cajeta_dbg_local (name, type,
// addr, alloc, ownership), then read the facets back through the host
// accessors __cajeta_dbg_local_alloc / _ownership. This pins the ABI the
// JIT'd codegen side emits against (DebugCodegen::emitDbgLocal) and the values
// classifyField produces at the call sites. No JIT compile.
//
#include <gtest/gtest.h>

#include <cstdint>

#include "cajeta/dbg/MemoryFacets.h"

using cajeta::dbg::AllocClass;
using cajeta::dbg::OwnershipRole;
using cajeta::dbg::MemoryFacets;
using cajeta::dbg::FieldFacetInputs;
using cajeta::dbg::classifyField;

extern "C" {
    void __cajeta_dbg_frame_enter(const char* func);
    void __cajeta_dbg_frame_leave(void);
    void __cajeta_dbg_local(const char* name, const char* type, void* addr,
                            uint8_t alloc, uint8_t ownership);
    void** __cajeta_dbg_top_ptr(void);
    uint8_t __cajeta_dbg_local_alloc(void* frame, int i);
    uint8_t __cajeta_dbg_local_ownership(void* frame, int i);
    const char* __cajeta_dbg_local_name(void* frame, int i);
    int __cajeta_dbg_frame_nlocals(void* frame);
}

namespace {
    // Register a local whose facet bytes come from classifyField, mirroring
    // exactly what the codegen call sites do.
    void registerLocal(const char* name, const char* type, void* addr,
                       const FieldFacetInputs& in) {
        MemoryFacets f = classifyField(in);
        __cajeta_dbg_local(name, type, addr,
                           static_cast<uint8_t>(f.alloc),
                           static_cast<uint8_t>(f.ownership));
    }
}

// A stack primitive: inline value, no drop entry, not a reference -> Stack
// allocation but Unknown ownership (a plain value isn't owner or borrow).
TEST(DbgLocalFacets, StackPrimitiveIsStackUnknown) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr) << "chain not clean at start";
    __cajeta_dbg_frame_enter("demo.F::m");
    int32_t v = 1;
    FieldFacetInputs in; in.isStackField = true;
    registerLocal("v", "int32", &v, in);

    void* top = *__cajeta_dbg_top_ptr();
    ASSERT_EQ(__cajeta_dbg_frame_nlocals(top), 1);
    EXPECT_STREQ(__cajeta_dbg_local_name(top, 0), "v");
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 0),
              static_cast<uint8_t>(AllocClass::Stack));
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 0),
              static_cast<uint8_t>(OwnershipRole::Unknown));

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

// An owned heap object: heap slot + a live drop entry -> Heap + Owner.
TEST(DbgLocalFacets, OwnedHeapObjectIsHeapOwner) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
    __cajeta_dbg_frame_enter("demo.F::m");
    void* slot = nullptr;
    FieldFacetInputs in; in.isHeapField = true; in.ownsDrop = true;
    registerLocal("o", "demo.Foo", &slot, in);

    void* top = *__cajeta_dbg_top_ptr();
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 0),
              static_cast<uint8_t>(AllocClass::Heap));
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 0),
              static_cast<uint8_t>(OwnershipRole::Owner));

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

// A borrow: heap slot, reference, no drop entry -> Heap + Borrow.
TEST(DbgLocalFacets, BorrowIsHeapBorrow) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
    __cajeta_dbg_frame_enter("demo.F::m");
    void* slot = nullptr;
    FieldFacetInputs in; in.isHeapField = true; in.isReference = true;
    registerLocal("b", "demo.Foo", &slot, in);

    void* top = *__cajeta_dbg_top_ptr();
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 0),
              static_cast<uint8_t>(AllocClass::Heap));
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 0),
              static_cast<uint8_t>(OwnershipRole::Borrow));

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

// An owning view (reference AND owns a drop entry): ownsDrop dominates, so it
// reads as Owner, not Borrow — the distinction CP7 must preserve.
TEST(DbgLocalFacets, OwningViewIsOwnerNotBorrow) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
    __cajeta_dbg_frame_enter("demo.F::m");
    void* slot = nullptr;
    FieldFacetInputs in;
    in.isHeapField = true; in.isReference = true; in.ownsDrop = true;
    registerLocal("view", "demo.Foo[]", &slot, in);

    void* top = *__cajeta_dbg_top_ptr();
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 0),
              static_cast<uint8_t>(OwnershipRole::Owner));

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

// Multiple locals keep their own facets, indexed independently.
TEST(DbgLocalFacets, PerLocalFacetsAreIndependent) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
    __cajeta_dbg_frame_enter("demo.F::m");
    int32_t prim = 0; void* owned = nullptr;
    FieldFacetInputs pin; pin.isStackField = true;
    FieldFacetInputs oin; oin.isHeapField = true; oin.ownsDrop = true;
    registerLocal("prim", "int32", &prim, pin);
    registerLocal("owned", "demo.Foo", &owned, oin);

    void* top = *__cajeta_dbg_top_ptr();
    ASSERT_EQ(__cajeta_dbg_frame_nlocals(top), 2);
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 0),
              static_cast<uint8_t>(AllocClass::Stack));
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 0),
              static_cast<uint8_t>(OwnershipRole::Unknown));
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 1),
              static_cast<uint8_t>(AllocClass::Heap));
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, 1),
              static_cast<uint8_t>(OwnershipRole::Owner));

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

// Out-of-range / null reads degrade to 0 (== Unknown), never crash.
TEST(DbgLocalFacets, OutOfRangeReadsUnknown) {
    EXPECT_EQ(__cajeta_dbg_local_alloc(nullptr, 0), 0);
    EXPECT_EQ(__cajeta_dbg_local_ownership(nullptr, 0), 0);

    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
    __cajeta_dbg_frame_enter("demo.F::m");
    void* top = *__cajeta_dbg_top_ptr();
    EXPECT_EQ(__cajeta_dbg_local_alloc(top, 5), 0);
    EXPECT_EQ(__cajeta_dbg_local_ownership(top, -1), 0);
    __cajeta_dbg_frame_leave();
}
