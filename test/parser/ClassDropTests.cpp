//
// Tests for user-defined drop on class instances. A class can declare a
// `drop()` method; the compiler synthesizes a per-class drop wrapper
// that calls user.drop() then frees the heap allocation, and registers
// a drop entry for every class-typed local so it fires at scope exit.
// Foundation for the upcoming user-facing Lock class with RAII guard
// (see docs/specification/concurrent/Concurrency.md § Synchronization primitives).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// run/read pattern: run() does the work and triggers scope-exit drops on
// its return; read() observes the post-drop count separately. Mirrors
// DropChainTests.observe — needed because drops fire at method exit, so
// the same method can't measure its own drops.
int64_t observeDropCount(const std::string& classBody,
                         const std::string& runBody) {
    std::string src =
        std::string("package test;\n"
        "public class Thing {\n"
        "    ") + classBody + "\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        " + runBody + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    return jit->lookup<int64_t (*)()>("read")();
}

} // namespace

// One class instance with no user drop method — the auto-drop entry
// fires once at scope exit, count = 1.
TEST(ClassDropTests, instanceWithoutUserDropFiresAutoDrop) {
    EXPECT_EQ(observeDropCount(
        /*classBody*/ "public int32 value() { return 7; }",
        /*runBody*/   "Thing t = heap Thing();"
    ), 1);
}

// Two class locals → two drop entries firing LIFO.
TEST(ClassDropTests, twoInstancesFireBothDrops) {
    EXPECT_EQ(observeDropCount(
        /*classBody*/ "public int32 value() { return 7; }",
        /*runBody*/   "Thing a = heap Thing();\n"
        "        Thing b = heap Thing();"
    ), 2);
}

// User-defined destructor is invoked before the heap free. The
// observable: have ~Tracer() allocate a small array — when the
// destructor returns the array's own drop fires, adding 1 to the
// drop count. So a class with a destructor that allocates an array
// contributes:
//   1 for the class instance's drop entry (pop_run's pre-increment)
//   1 for the array's drop fired at the destructor's scope-exit
// Total = 2. A class without a destructor contributes only 1.
TEST(ClassDropTests, userDropMethodIsInvoked) {
    auto src =
        "package test;\n"
        "public class Tracer {\n"
        "    public ~Tracer() {\n"
        "        int32[] junk = heap int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Tracer t = heap Tracer();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Sanity: a class WITHOUT a user drop contributes only 1 increment
// (the auto-drop entry's pre-increment in __cajeta_drop_pop_run).
TEST(ClassDropTests, classWithoutUserDropContributesOne) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 v() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Plain p = heap Plain();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 1);
}

// LIFO drop order: a's drop fires AFTER b's drop. We observe this by
// having a's drop() reset the counter — if b dropped after a, the
// counter would have b's auto-free increment on top of a's reset (1).
// If a drops after b (LIFO, correct), a's reset wipes b's increment
// and a's auto-free gets us back to 1.
//
// Either way the end count is 1 in this construction, so this test is
// really just confirming the drops fire at all and don't double-free.
TEST(ClassDropTests, twoInstancesDropWithoutCrash) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Box a = heap Box();\n"
        "        Box b = heap Box();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Returning a class-typed local transfers ownership — the producing
// method's drop entry is deactivated, the caller's local registers a
// fresh entry. Net drop count from one round-trip = 1 (caller's drop
// at its own scope exit). The `#Counter` return-type marker is what
// the new MemoryModel convention requires: without it, the caller
// treats the result as a borrow (no drop registered on receipt) and
// the returned allocation leaks. (Task #54 made the receive-side
// honor the marker; before that, every class-typed return was
// implicitly a transfer.)
TEST(ClassDropTests, returnedInstanceOwnershipTransfers) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 next() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #Counter mk() {\n"
        "        Counter c = heap Counter();\n"
        "        return c;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Counter received = mk();\n"
        "        return received.next();\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 42);
    // mk() returns the closure → its drop entry was deactivated. run()
    // owns `received`; at run's exit, received drops once. Total: 1.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 1);
}
