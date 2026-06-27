// Unit 1 (benchmark-fidelity-plan) — the cumulative allocation-bytes counter that
// backs Cajeta.allocatedBytes(), so the profile harness can emit a real alloc_bytes
// column and the report's Memory tab populates for cajeta. Tested directly against
// the runtime C entry points (same style as ArenaRuntimeTests).
#include <gtest/gtest.h>
#include <cstdint>

extern "C" {
    // The new counter under test.
    int64_t __cajeta_total_allocated_bytes(void);
    // Allocation entry points that must feed the counter.
    void* __cajeta_new_array(uint64_t elem_size, uint64_t total_count);
    void* __cajeta_new_array_header(uint64_t header_size, uint64_t elem_size, uint64_t count);
    void* __cajeta_new_array_header_uninit(uint64_t header_size, uint64_t elem_size, uint64_t count);
    void* __cajeta_alloc(uint64_t size);
    void* __cajeta_alloc_uninit(uint64_t size);
}

// 1.a.1 — every allocation path increments the cumulative byte counter by at
// least the bytes requested.
TEST(AllocatedBytes, newArrayCounts) {
    int64_t base = __cajeta_total_allocated_bytes();
    void* p = __cajeta_new_array(8, 1000);   // 8000 bytes
    ASSERT_NE(p, nullptr);
    EXPECT_GE(__cajeta_total_allocated_bytes() - base, 8000);
}

TEST(AllocatedBytes, headerArraysCount) {
    int64_t base = __cajeta_total_allocated_bytes();
    void* a = __cajeta_new_array_header(8, 4, 100);          // 8 + 400
    void* b = __cajeta_new_array_header_uninit(8, 1, 256);   // 8 + 256
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_GE(__cajeta_total_allocated_bytes() - base, (8 + 400) + (8 + 256));
}

TEST(AllocatedBytes, genericAllocCounts) {
    int64_t base = __cajeta_total_allocated_bytes();
    void* p = __cajeta_alloc(256);
    void* q = __cajeta_alloc_uninit(512);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(q, nullptr);
    EXPECT_GE(__cajeta_total_allocated_bytes() - base, 256 + 512);
}

// 1.a.2 (counter half) — cumulative + monotonic: no allocation between two reads
// leaves the counter unchanged (a zero-alloc measured window reports 0 delta).
TEST(AllocatedBytes, monotonicNoAllocFlat) {
    int64_t a = __cajeta_total_allocated_bytes();
    int64_t b = __cajeta_total_allocated_bytes();
    EXPECT_EQ(a, b);
    void* p = __cajeta_alloc(64);
    ASSERT_NE(p, nullptr);
    int64_t c = __cajeta_total_allocated_bytes();
    EXPECT_GE(c - b, 64);
    int64_t d = __cajeta_total_allocated_bytes();
    EXPECT_EQ(c, d);   // flat again with no intervening allocation
}
