//
// User-facing Lock and LockGuard classes — the cajeta.concurrent API
// that wraps the low-level lock intrinsics with RAII semantics.
// LockGuard's user-defined drop() calls release; the class-drop
// infrastructure auto-fires it at scope exit. See docs/specification/concurrent/Concurrency.md §
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

// tryAcquire on an uncontended lock succeeds (returns 1). The caller
// releases manually; the lock's auto-drop at method exit destroys the
// mutex.

// While the calling thread already holds the lock, a second tryAcquire
// fails (returns 0). Verifies dispatch through the user-facing class
// reaches the right pthread_mutex behaviour.

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
        "        LockGuard g #= lock.acquire();\n"
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

// Manual acquire/release pairs through the user-facing API. No
// LockGuard — exercises the API surface without depending on RAII
// release order.
