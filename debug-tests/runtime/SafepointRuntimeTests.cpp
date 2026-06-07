//
// Tests for the runtime debug-safepoint counter (CP2), exercising the native
// runtime object linked into this binary (the host copy — the same one the
// JIT'd code's embedded-bitcode copy mirrors). Verifies the hook counts calls
// and resets cleanly.
//
#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
    void __cajeta_dbg_safepoint(int32_t loc_id);
    long __cajeta_dbg_safepoint_count(void);
    void __cajeta_dbg_reset_safepoint_count(void);
}

TEST(SafepointRuntime, CountsCalls) {
    __cajeta_dbg_reset_safepoint_count();
    EXPECT_EQ(__cajeta_dbg_safepoint_count(), 0);
    __cajeta_dbg_safepoint(0);
    __cajeta_dbg_safepoint(1);
    __cajeta_dbg_safepoint(2);
    EXPECT_EQ(__cajeta_dbg_safepoint_count(), 3);
}

TEST(SafepointRuntime, ResetReturnsToZero) {
    __cajeta_dbg_safepoint(7);
    EXPECT_GT(__cajeta_dbg_safepoint_count(), 0);
    __cajeta_dbg_reset_safepoint_count();
    EXPECT_EQ(__cajeta_dbg_safepoint_count(), 0);
}
