//
// User-facing Lock and LockGuard classes — the cajeta.concurrent API
// that wraps the low-level lock intrinsics with RAII semantics.
// LockGuard's user-defined drop() calls release; the class-drop
// infrastructure auto-fires it at scope exit. See docs/stdlib/Concurrency.md §
// Synchronization primitives.
//
// As of R7-A these classes are part of the preloaded standard library
// (runtime/src/cajeta/concurrent/{Lock,LockGuard}.cajeta), auto-embedded
// into the compiler. The tests now `import cajeta.concurrent.*` rather
// than inlining the source — exercising the real stdlib types and the
// embed/auto-load path end-to-end.
//
// LockGuard holds the raw handle (not a back-reference to the Lock),
// so its drop() reaches the runtime helper directly. The lifetime
// contract is "guard <= lock". tryAcquire returns an int32 (1=got,
// 0=held); a successful tryAcquire pairs with a manual release at the
// caller (overloaded constructors aren't supported yet, so a guard-
// returning tryAcquire is deferred).
//
// Cajeta's drop chain is method-scoped, not block-scoped: a LockGuard
// declared inside an inner `{ }` doesn't release until the method
// returns. That shapes these tests — RAII release on inner-scope exit
// is verified indirectly via a producer-method whose return triggers
// the chain.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile a complete source unit: import the stdlib Lock + LockGuard,
// plus the user-supplied class body for D.
std::string lockTestSource(const std::string& dBody) {
    return std::string("package test;\n")
        + "import cajeta.concurrent.Lock;\n"
        + "import cajeta.concurrent.LockGuard;\n"
        + "public final class D {\n" + dBody + "}\n";
}

int32_t runI32(const std::string& dBody) {
    auto jit = CajetaJit::compile(lockTestSource(dBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Smoke: construct + destroy. Lock's ctor calls lockNew; its
// synthesized drop wrapper calls user.drop() (which calls lockDestroy),
// then __cajeta_free.
TEST(LockClassTests, constructAndDestroy) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Lock lock = heap Lock();\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

// tryAcquire on an uncontended lock succeeds (returns 1). The caller
// releases manually; the lock's auto-drop at method exit destroys the
// mutex.
TEST(LockClassTests, tryAcquireUncontendedSucceeds) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Lock lock = heap Lock();\n"
        "        int32 got = lock.tryAcquire();\n"
        "        if (got == 1) lock.releaseLock();\n"
        "        return got;\n"
        "    }\n"
    ), 1);
}

// While the calling thread already holds the lock, a second tryAcquire
// fails (returns 0). Verifies dispatch through the user-facing class
// reaches the right pthread_mutex behaviour.
TEST(LockClassTests, tryAcquireWhileHeldFails) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Lock lock = heap Lock();\n"
        "        LockGuard held = lock.acquire();\n"
        "        int32 second = lock.tryAcquire();\n"
        "        return second;\n"
        "    }\n"
    ), 0);
}

// RAII verification through method boundaries: a helper method
// acquires the lock and returns — the method-scoped drop chain fires
// the LockGuard's drop (= release) on return, BEFORE the Lock is
// destroyed. Then a fresh Lock in run() probes nothing about that
// specific mutex, but the helper completing without deadlock or
// invalid-free confirms the drop sequence is well-ordered.
TEST(LockClassTests, guardDropAtMethodExitReleasesBeforeDestroy) {
    EXPECT_EQ(runI32(
        "    public static int32 acquireAndReturn() {\n"
        "        Lock lock = heap Lock();\n"
        "        LockGuard g = lock.acquire();\n"
        "        return 0;\n"
        "        // At return: g drops first (release), lock drops\n"
        "        // (destroy). If the order were reversed, destroy on\n"
        "        // a still-held mutex would crash.\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 unused = acquireAndReturn();\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

// Two independent Lock instances coexist without interference.
TEST(LockClassTests, twoIndependentLocks) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Lock a = heap Lock();\n"
        "        Lock b = heap Lock();\n"
        "        LockGuard ga = a.acquire();\n"
        "        int32 bFree = b.tryAcquire();\n"
        "        if (bFree == 1) b.releaseLock();\n"
        "        return bFree;\n"
        "    }\n"
    ), 1);
}

// Manual acquire/release pairs through the user-facing API. No
// LockGuard — exercises the API surface without depending on RAII
// release order.
TEST(LockClassTests, repeatedManualAcquireRelease) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Lock lock = heap Lock();\n"
        "        int32 sum = 0;\n"
        "        int32 got1 = lock.tryAcquire();\n"
        "        if (got1 == 1) {\n"
        "            sum = sum + 10;\n"
        "            lock.releaseLock();\n"
        "        }\n"
        "        int32 got2 = lock.tryAcquire();\n"
        "        if (got2 == 1) {\n"
        "            sum = sum + 20;\n"
        "            lock.releaseLock();\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
    ), 30);
}
