//
// cajeta.concurrent.Mutex<T> — the fused mutual-exclusion + protected-data
// primitive (R7-C). The mutex owns a single T; the only way to touch it is
// through withLock, whose closure is the critical section (Java
// synchronized(obj){...} shape). get() snapshots the value under the lock.
//
// These tests load Mutex<T> from the embedded standard library
// (runtime/src/cajeta/concurrent/Mutex.cajeta) — they only `import` it,
// exercising the real stdlib type. Single-threaded (main-thread,
// uncontended) JIT runs: lockAcquire takes the lock immediately, the
// closure runs, the method-scoped LockGuard drop releases.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string mutexTestSource(const std::string& dBody) {
    return std::string("package test;\n")
        + "import cajeta.concurrent.Mutex;\n"
        + "public final class D {\n" + dBody + "}\n";
}

int32_t runI32(const std::string& dBody) {
    auto jit = CajetaJit::compile(mutexTestSource(dBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Smoke: construct + destroy a Mutex<int32>. Ctor calls lockNew + stores
// the initial value; the synthesized drop wrapper calls ~Mutex (lockDestroy)
// then frees.
TEST(MutexTests, constructAndDestroy) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(7);\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

// get() returns the initial protected value under the lock.
TEST(MutexTests, getReturnsInitial) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(42);\n"
        "        return m.get();\n"
        "    }\n"
    ), 42);
}

// withLock mutates the protected value and stores the closure's result;
// get() reads it back.
TEST(MutexTests, withLockMutatesProtectedValue) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(0);\n"
        "        m.withLock((int32 v) -> v + 5);\n"
        "        return m.get();\n"
        "    }\n"
    ), 5);
}

// Repeated withLock calls accumulate — each takes the lock, applies the
// closure to the current value, releases.
TEST(MutexTests, repeatedWithLockAccumulates) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(0);\n"
        "        m.withLock((int32 v) -> v + 5);\n"
        "        m.withLock((int32 v) -> v + 10);\n"
        "        m.withLock((int32 v) -> v * 2);\n"
        "        return m.get();\n"
        "    }\n"
    ), 30);
}

// Two independent Mutex<int32> instances don't interfere.
TEST(MutexTests, twoIndependentMutexes) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> a = heap Mutex<int32>(1);\n"
        "        Mutex<int32> b = heap Mutex<int32>(100);\n"
        "        a.withLock((int32 v) -> v + 1);\n"
        "        b.withLock((int32 v) -> v + 1);\n"
        "        return a.get() + b.get();\n"
        "    }\n"
    ), 103);
}

// withLockWhen whose predicate already holds runs immediately (no wait,
// no fiber scheduling) — main-thread happy path.
TEST(MutexTests, withLockWhenConditionAlreadyTrue) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(20);\n"
        "        m.withLockWhen((int32 v) -> v >= 10, (int32 v) -> v + 1);\n"
        "        return m.get();\n"
        "    }\n"
    ), 21);
}

// Producer/consumer across fibers: the consumer blocks in withLockWhen
// until value >= 10 (parks on the condvar, releasing the lock), the
// producer sets value to 10 and notifies, the consumer wakes, re-checks,
// and increments. Exercises condvar wait/notify + lock hand-off under the
// single cooperative carrier.
TEST(MutexTests, withLockWhenProducerConsumer) {
    EXPECT_EQ(runI32(
        "    public static async int32 consumer(Mutex<int32> m) {\n"
        "        m.withLockWhen((int32 v) -> v >= 10, (int32 v) -> v + 1);\n"
        "        return 0;\n"
        "    }\n"
        "    public static async int32 producer(Mutex<int32> m) {\n"
        "        m.withLock((int32 v) -> 10);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Mutex<int32> m = heap Mutex<int32>(0);\n"
        "        scope {\n"
        "            spawn consumer(m);\n"
        "            spawn producer(m);\n"
        "        }\n"
        "        return m.get();\n"
        "    }\n"
    ), 11);
}
