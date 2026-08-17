// lazy-codegen 2.1.5 — the emission-mode toggle.
//
// This exists as its own unit because the OBVIOUS implementation is broken and
// already shipped once: `CAJETA_TWO_STAGE_PARSE` reads getenv into a function
// -local static, so the value is fixed at first call and no test can A/B it in
// one process. Today's SLL fix could not be regression-tested by flipping a
// flag for exactly that reason.
//
// So the contract here is: the environment SEEDS the mode, it does not LOCK it.

#include "gtest/gtest.h"

#include "cajeta/jit/LazyCodegen.h"

using cajeta::lazyCodegenEnabled;
using cajeta::setLazyCodegenEnabled;

namespace {

// Every test restores the mode, so ordering can never leak.
struct ModeGuard {
    bool saved = lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

} // namespace

// Unit 2 lands the generator behind the eager path: until Unit 4 flips it, a
// process that sets nothing must behave exactly as it does today.
TEST(LazyCodegenModeTests, defaultsToEager) {
    ModeGuard guard;
    setLazyCodegenEnabled(false);
    EXPECT_FALSE(lazyCodegenEnabled());
}

// THE point of 2.1.5. A getenv-into-static toggle passes every other test in
// this file and still fails this one.
TEST(LazyCodegenModeTests, togglesInProcess) {
    ModeGuard guard;

    setLazyCodegenEnabled(true);
    EXPECT_TRUE(lazyCodegenEnabled());

    setLazyCodegenEnabled(false);
    EXPECT_FALSE(lazyCodegenEnabled());

    // And back — a one-way latch would pass the two checks above.
    setLazyCodegenEnabled(true);
    EXPECT_TRUE(lazyCodegenEnabled());
}

// The guard pattern the rest of the suite relies on: a flip must not outlive
// its scope, or one test silently decides the mode for the next.
TEST(LazyCodegenModeTests, guardRestoresPriorMode) {
    setLazyCodegenEnabled(false);
    {
        ModeGuard guard;
        setLazyCodegenEnabled(true);
        EXPECT_TRUE(lazyCodegenEnabled());
    }
    EXPECT_FALSE(lazyCodegenEnabled());
}

// Reading the mode must be free of side effects — the generator consults it on
// every tryToGenerate, so a lazily-initialising read would be a data race the
// moment ORC calls in from a materialization thread.
TEST(LazyCodegenModeTests, readIsIdempotent) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(lazyCodegenEnabled());
    }
}
