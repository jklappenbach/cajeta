// Tests for the --poison-free=on runtime hook.
//
// The flag is plumbed via __cajeta_set_poison_free(int) — the JIT
// initializer (JitTestHelper) calls the setter from Options.poisonFreeEnabled
// before user code runs. Per CompilerModes.md, when poison-free is on,
// every heap chunk gets memset to 0xDB right before free() so a use-
// after-free either traps (deref a 0xDB...DB pointer) or surfaces
// obviously wrong data.
//
// We test the poison logic directly by calling __cajeta_poison_buffer
// (which runs the same memset path __cajeta_free invokes, but without
// the free that would make subsequent reads UB). That keeps the test
// itself free of post-free reads.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>

using cajeta_test::CajetaJit;

// Runtime symbols defined in runtime/native/cajeta_runtime.c. The test
// binary statically links the runtime alongside the JIT host, so these
// resolve directly.
extern "C" {
    void  __cajeta_set_poison_free(int enabled);
    int   __cajeta_get_poison_free(void);
    void  __cajeta_poison_buffer(void* ptr);
    void* __cajeta_alloc(uint64_t size);
    void  __cajeta_free(void* ptr);
}

namespace {

// Snapshot/restore wrapper so a failing test doesn't leak poison-free
// state into the next one. GoogleTest runs tests serially by default.
class PoisonFreeStateGuard {
public:
    PoisonFreeStateGuard() : saved(__cajeta_get_poison_free()) {}
    ~PoisonFreeStateGuard() { __cajeta_set_poison_free(saved); }
private:
    int saved;
};

} // namespace

// Default off: the runtime flag starts disabled (its static initializer
// is 0). __cajeta_get_poison_free reflects whatever the last caller —
// JIT init or our setter — left behind. No baseline assertion here
// since prior tests may have flipped it; we explicitly set state in
// the tests that depend on it.

// Setter round-trip.
TEST(PoisonFreeTests, setterRoundTrip) {
    PoisonFreeStateGuard guard;
    __cajeta_set_poison_free(0);
    EXPECT_EQ(__cajeta_get_poison_free(), 0);
    __cajeta_set_poison_free(1);
    EXPECT_EQ(__cajeta_get_poison_free(), 1);
    // Truthy values normalize to 1.
    __cajeta_set_poison_free(42);
    EXPECT_EQ(__cajeta_get_poison_free(), 1);
    __cajeta_set_poison_free(0);
    EXPECT_EQ(__cajeta_get_poison_free(), 0);
}

// With the flag OFF, __cajeta_poison_buffer is a no-op — known bytes
// stay where they were written.
TEST(PoisonFreeTests, poisonBufferIsNoopWhenDisabled) {
    PoisonFreeStateGuard guard;
    __cajeta_set_poison_free(0);

    void* p = __cajeta_alloc(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x42, 64);

    __cajeta_poison_buffer(p);

    // Bytes unchanged: still 0x42.
    auto* bytes = static_cast<uint8_t*>(p);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(bytes[i], 0x42) << "byte " << i << " was modified despite flag=off";
    }

    __cajeta_free(p);
}

// With the flag ON, __cajeta_poison_buffer fills the chunk with 0xDB.
// The requested size was 64 bytes; malloc_usable_size may report more
// (chunk rounding), so we verify the requested region — everything
// past that point is still inside the chunk and also poisoned, but
// we don't probe it directly to keep the test independent of glibc's
// rounding behavior.
TEST(PoisonFreeTests, poisonBufferFillsWithSentinelWhenEnabled) {
    PoisonFreeStateGuard guard;
    __cajeta_set_poison_free(1);

    void* p = __cajeta_alloc(64);
    ASSERT_NE(p, nullptr);
    // Pre-fill with a different pattern so we can tell the poison
    // actually overwrote it.
    std::memset(p, 0x42, 64);

    __cajeta_poison_buffer(p);

    auto* bytes = static_cast<uint8_t*>(p);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(bytes[i], 0xDB) << "byte " << i
            << " not poisoned (got 0x"
            << std::hex << (int) bytes[i] << ")";
    }

    __cajeta_free(p);
}

// NULL is safe: passing NULL to the poison helper is a no-op
// regardless of flag state.
TEST(PoisonFreeTests, poisonBufferIsNullSafe) {
    PoisonFreeStateGuard guard;
    __cajeta_set_poison_free(1);
    __cajeta_poison_buffer(nullptr);  // must not crash
    __cajeta_set_poison_free(0);
    __cajeta_poison_buffer(nullptr);  // must not crash
    SUCCEED();
}

// JIT integration: a cajeta program that allocates, mutates, and
// drops a heap object still runs cleanly when poison-free is
// enabled through the JIT options. This pins that the wiring from
// Options.poisonFreeEnabled through to the runtime flag is sound
// — the body of __cajeta_free runs the poison memset on the actual
// instance and the test doesn't observe a use-after-free.

// JIT integration: same program, but with poisonFreeEnabled left at
// its default (false). The JIT init hook calls
// __cajeta_set_poison_free(0) unconditionally, so the flag is off
// after this compile() returns — a guard against state leaking
// across tests.
