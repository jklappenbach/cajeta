// Unit 1 of the slices plan (agents/cajeta/slices-plan.md): the `shared`
// ownership substrate — count-word sign-bit flag + side-table refcount, with
// the final release routed through the live-set claim (slice-spec §3).
// Host-level tests against the native runtime linked into cajeta_lib.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
void __cajeta_live_set_add(void* p);
int __cajeta_live_set_claim(void* p);
void __cajeta_live_set_go_multithreaded(void);

void __cajeta_shared_promote(void* base, int64_t stakes);
void __cajeta_shared_retain(void* base);
int __cajeta_shared_release(void* base);
int __cajeta_shared_owner_drop(void* base);
int __cajeta_shared_is(const void* base);
int64_t __cajeta_shared_masked_count(const void* base);
int64_t __cajeta_shared_rc(void* base);
int64_t __cajeta_shared_population(void);
}

namespace {

// A {i64 count; data} heap buffer registered with the live-set, as the
// runtime's array allocator produces.
void* mkBuffer(int64_t count) {
    void* buf = malloc(sizeof(int64_t) + (size_t) count);
    *(int64_t*) buf = count;
    memset((char*) buf + sizeof(int64_t), 0xAB, (size_t) count);
    __cajeta_live_set_add(buf);
    return buf;
}

}  // namespace

// 1.1.1 — the sign-bit flag round-trips and the masked count is unchanged;
// an unmasked reader of a shared buffer sees a NEGATIVE count (fails closed).
TEST(SharedStateTests, sharedBitRoundTrip) {
    void* buf = mkBuffer(1024);
    EXPECT_FALSE(__cajeta_shared_is(buf));
    EXPECT_EQ(__cajeta_shared_masked_count(buf), 1024);

    __cajeta_shared_promote(buf, 2);
    EXPECT_TRUE(__cajeta_shared_is(buf));
    EXPECT_EQ(__cajeta_shared_masked_count(buf), 1024);
    EXPECT_LT(*(int64_t*) buf, 0);  // unmasked readers fail closed

    EXPECT_EQ(__cajeta_shared_release(buf), 0);   // owner stake
    EXPECT_EQ(__cajeta_shared_release(buf), 1);   // last stake -> claimed
    free(buf);
}

// 1.1.2 — promote/retain/release lifecycle: freed exactly once, through the
// live-set claim; a racing claim (an auto-field-drop winning) no-ops the
// final release.
TEST(SharedStateTests, sideTablePromoteCountFree) {
    // Clean lifecycle: rc 2 -> 3 -> 2 -> 1 -> 0 frees via claim.
    void* a = mkBuffer(64);
    __cajeta_shared_promote(a, 2);
    __cajeta_shared_retain(a);
    EXPECT_EQ(__cajeta_shared_rc(a), 3);
    EXPECT_EQ(__cajeta_shared_release(a), 0);
    EXPECT_EQ(__cajeta_shared_release(a), 0);
    EXPECT_EQ(__cajeta_shared_rc(a), 1);
    int64_t popBefore = __cajeta_shared_population();
    EXPECT_EQ(__cajeta_shared_release(a), 1);     // last stake: claim + free
    EXPECT_EQ(__cajeta_shared_population(), popBefore - 1);
    EXPECT_EQ(__cajeta_live_set_claim(a), 0);     // already claimed: no double free
    free(a);

    // Racing duplicate drop: an external claim wins; the final release no-ops.
    void* b = mkBuffer(64);
    __cajeta_shared_promote(b, 2);
    EXPECT_EQ(__cajeta_shared_release(b), 0);
    EXPECT_EQ(__cajeta_live_set_claim(b), 1);     // racer claims first
    EXPECT_EQ(__cajeta_shared_release(b), 0);     // rc hit 0 but claim failed: no free
    free(b);

    // Chained promote (slice of an already-shared buffer) retains, not resets.
    void* c = mkBuffer(64);
    __cajeta_shared_promote(c, 2);
    __cajeta_shared_promote(c, 2);                // == retain
    EXPECT_EQ(__cajeta_shared_rc(c), 3);
    EXPECT_EQ(__cajeta_shared_release(c), 0);
    EXPECT_EQ(__cajeta_shared_release(c), 0);
    EXPECT_EQ(__cajeta_shared_release(c), 1);
    free(c);
}

// 1.1.3 — a never-promoted buffer's owner-drop is exactly today's claim
// (first caller frees, second no-ops); no side-table traffic.
TEST(SharedStateTests, unsharedDropUnchanged) {
    void* buf = mkBuffer(256);
    int64_t pop = __cajeta_shared_population();
    EXPECT_EQ(__cajeta_shared_owner_drop(buf), 1);   // bit clear -> plain claim
    EXPECT_EQ(__cajeta_shared_owner_drop(buf), 0);   // duplicate drop no-ops
    EXPECT_EQ(__cajeta_shared_population(), pop);    // side table untouched
    free(buf);
}

// 1.1.5 — an address never registered (static storage) is never freed:
// claim and release both refuse it.
TEST(SharedStateTests, staticNeverCounted) {
    static int64_t staticBuf[9] = {64, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_FALSE(__cajeta_shared_is(staticBuf));
    EXPECT_EQ(__cajeta_shared_release(staticBuf), 0);
    EXPECT_EQ(__cajeta_shared_owner_drop(staticBuf), 0);  // not in live-set
}

// 1.1.4 — after the mt flip, concurrent retain/release from multiple threads
// keeps the count exact and frees exactly once. (The flip is one-way and
// process-global — correctness-preserving for every later test, which simply
// takes the mutex path.) Runs LAST in this suite by naming convention.
TEST(SharedStateTests, zMtEscalation) {
    __cajeta_live_set_go_multithreaded();
    void* buf = mkBuffer(128);
    __cajeta_shared_promote(buf, 2);

    constexpr int kThreads = 4;
    constexpr int kIters = 5000;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; t++) {
        ts.emplace_back([&] {
            for (int i = 0; i < kIters; i++) {
                __cajeta_shared_retain(buf);
                ASSERT_EQ(__cajeta_shared_release(buf), 0);
            }
        });
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(__cajeta_shared_rc(buf), 2);
    EXPECT_EQ(__cajeta_shared_release(buf), 0);
    EXPECT_EQ(__cajeta_shared_release(buf), 1);
    free(buf);
}
