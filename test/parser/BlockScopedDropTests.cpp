//
// Tests for block-scoped drops. Each `{ ... }` is its own drop frame:
// owned locals declared inside fire their destructors at the closing
// `}`, not at the enclosing method's exit. Verifies the lock-twice-in-
// a-method pattern that cajeta-docs/stdlib/Thread.md's RAII guard story depends on.
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

int64_t observeDrops(const std::string& body) {
    std::string src =
        std::string("package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        ") + body + "\n"
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

// Inner-block local fires its drop at the closing `}`, before the
// outer block continues. Observable via a probe-after-block that sees
// the count already incremented.
TEST(BlockScopedDropTests, innerBlockArrayDropsAtClosingBrace) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        {\n"
        "            int32[] tmp = heap int32[2];\n"
        "        }\n"  // tmp's drop fires HERE
        "        int64 mid = Cajeta.dropCount();\n"
        "        return (int32) mid;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Two sequential inner blocks each drop their own local at their own
// closing brace — count = 2 by the time we observe.
TEST(BlockScopedDropTests, sequentialBlocksEachDropOnExit) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        {\n"
        "            int32[] a = heap int32[1];\n"
        "        }\n"
        "        {\n"
        "            int32[] b = heap int32[1];\n"
        "        }\n"
        "        int64 c = Cajeta.dropCount();\n"
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Nested blocks: inner fires first, then outer at its own closing.
// At the probe between inner-end and outer-end, count = 1; at the
// final read (post outer-end), count = 2.
TEST(BlockScopedDropTests, nestedBlocksFireInnerFirstThenOuter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int64 afterInner = 0;\n"
        "        {\n"
        "            int32[] outer = heap int32[1];\n"
        "            {\n"
        "                int32[] inner = heap int32[1];\n"
        "            }\n"  // inner fires HERE → count = 1
        "            afterInner = Cajeta.dropCount();\n"
        "        }\n"  // outer fires HERE → count = 2
        "        int64 afterOuter = Cajeta.dropCount();\n"
        "        return (int32) (afterInner * 10 + afterOuter);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);  // 1*10 + 2
}

// Method-level locals still drop at method exit. observeDrops fires
// run()'s return → method-body block fires its frame → count visible
// to read().
TEST(BlockScopedDropTests, methodBodyLocalDropsAtMethodExit) {
    EXPECT_EQ(observeDrops(
        "int32[] arr = heap int32[1];"
    ), 1);
}

// The lock-twice-in-a-method pattern from cajeta-docs/stdlib/Thread.md. Pre-block-
// scoped-drops this deadlocked: the first LockGuard didn't release
// until method exit, so the second acquire blocked on a still-held
// mutex. With block-scoped drops, g releases at its closing `}` and
// the second acquire succeeds.
TEST(BlockScopedDropTests, twoBackToBackCriticalSections) {
    auto src =
        "package test;\n"
        "public class LockGuard {\n"
        "    public pointer handle;\n"
        "    public LockGuard(pointer handle) { this.handle = handle; }\n"
        "    public ~LockGuard() { Cajeta.lockRelease(this.handle); }\n"
        "}\n"
        "public class Lock {\n"
        "    private pointer handle;\n"
        "    public Lock() { this.handle = Cajeta.lockNew(); }\n"
        "    public ~Lock() { Cajeta.lockDestroy(this.handle); }\n"
        "    public #LockGuard acquire() {\n"
        "        Cajeta.lockAcquire(this.handle);\n"
        "        return heap LockGuard(this.handle);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Lock lock = heap Lock();\n"
        "        int32 step1 = 0;\n"
        "        int32 step2 = 0;\n"
        "        {\n"
        "            LockGuard g = lock.acquire();\n"
        "            step1 = 10;\n"
        "        }\n"
        "        {\n"  // would deadlock without block-scoped drops
        "            LockGuard g = lock.acquire();\n"
        "            step2 = 20;\n"
        "        }\n"
        "        return step1 + step2;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Early return fires every active frame in LIFO order — locals
// declared at any nesting level get dropped before the return
// instruction. Count should be 2: outer + inner both drop on the
// return path.
TEST(BlockScopedDropTests, earlyReturnFiresAllEnclosingFrames) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 worker() {\n"
        "        int32[] outer = heap int32[1];\n"
        "        {\n"
        "            int32[] inner = heap int32[1];\n"
        "            return 0;\n"  // fires inner + outer before ret
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 v = worker();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}
