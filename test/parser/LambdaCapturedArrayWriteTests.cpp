// Regression coverage for lambda writes to an array-field of a
// captured class.
//
// Originally classified as a separate compiler gap (the
// "LLVM alloca-cast crash on lambda writes to captured-class array
// field" item that lived briefly as P1 #2): inside a lambda body,
// `cap.arr[i] = x` (where `cap` is captured and `arr` is a
// class-typed array field) tripped a dyn_cast<AllocaInst> assertion
// during codegen. Investigation showed this was a downstream
// symptom of the lambda-capture walker gap fixed in commit bad612a:
// pre-fix, when the write lived inside a nested block (the
// correlation-test if-branch shape), the walker silently skipped
// `cap` from the captures-struct, codegen got a null receiver, and
// the array-element store assertion fired.
//
// The walker fix made `cap` capture correctly, so the
// `cap.arr[i] = x` lowering succeeds end-to-end with no further
// code changes. The tests below pin five increasingly demanding
// shapes (direct call, sequential bump, conditional inside if,
// inside a parallel findFirst predicate, read-modify-write of the
// slot, inside a for-loop body) so the bug can't quietly
// reintroduce itself if the walker pathway regresses.

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

constexpr const char* kProbeWithArray =
    "package test;\n"
    "public class Probe {\n"
    "    public int32[] log;\n"
    "    public int32 next;\n"
    "    public Probe(int32 capacity) {\n"
    "        this.log = new int32[capacity];\n"
    "        this.next = 0;\n"
    "    }\n"
    "}\n";

} // namespace

// Direct call: lambda captures `p` and writes p.log[idx] = x at top
// level of the lambda body. Crashes pre-fix with
// "dyn_cast<AllocaInst>" assertion failure during codegen of the
// array-element store.
TEST(LambdaCapturedArrayWriteTests, directWriteToCapturedClassArrayField) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Probe p = new Probe(4);\n"
        "        (int32, int32) -> void fn = (int32 idx, int32 x) -> {\n"
        "            p.log[idx] = x;\n"
        "        };\n"
        "        fn(0, 11);\n"
        "        fn(1, 22);\n"
        "        fn(2, 31);\n"
        "        return p.log[0] + p.log[1] + p.log[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 64);
}

// Same write but explicitly index-stepped via a captured counter
// field on the same Probe: the lambda writes p.log[p.next] and then
// bumps p.next. Mirrors how a correlation-test side channel would
// log values without coordinating an external index variable.
TEST(LambdaCapturedArrayWriteTests, sequentialWriteAndBumpCounter) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Probe p = new Probe(4);\n"
        "        (int32) -> void fn = (int32 x) -> {\n"
        "            p.log[p.next] = x;\n"
        "            p.next = p.next + 1;\n"
        "        };\n"
        "        fn(7);\n"
        "        fn(8);\n"
        "        fn(9);\n"
        "        // returns p.next * 100 + sum(log[0..2]) so a regression\n"
        "        // is visible in either dimension (count or values)\n"
        "        return p.next * 100 + p.log[0] + p.log[1] + p.log[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 324);
}

// Write happens inside an if-branch (nested block). Exercises both
// the array-field-store fix and the previously-fixed nested-block
// capture walker together — these were the two layers the
// correlation tests had to circumvent.
TEST(LambdaCapturedArrayWriteTests, conditionalWriteInsideIfBranch) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Probe p = new Probe(8);\n"
        "        (int32) -> void fn = (int32 x) -> {\n"
        "            if (x % 2 == 0) {\n"
        "                p.log[p.next] = x;\n"
        "                p.next = p.next + 1;\n"
        "            }\n"
        "        };\n"
        "        fn(1);\n"
        "        fn(2);\n"
        "        fn(3);\n"
        "        fn(4);\n"
        "        fn(6);\n"
        "        // p.log[0..2] = [2, 4, 6]; p.next == 3.\n"
        "        return p.next * 100 + p.log[0] + p.log[1] + p.log[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 312);
}

// Write inside the predicate of a parallel findFirst — the
// original shape the correlation tests stripped out. If the bug
// was specific to the parallel-terminal lowering path (not just
// the array-field-store codegen), this would be where it
// reappears.
TEST(LambdaCapturedArrayWriteTests, writeInsidePredicateOfParallelFindFirst) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe(8);\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> {\n"
        "                                  if (x == 73) {\n"
        "                                      p.log[p.next] = x;\n"
        "                                      p.next = p.next + 1;\n"
        "                                      return true;\n"
        "                                  }\n"
        "                                  return false;\n"
        "                              });\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        if (p.next < 1) { return -2; }\n"
        "        if (p.log[0] != o.get()) { return -3; }\n"
        "        return p.log[0];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 73);
}

// Read-modify-write on the array slot — exercises the lowering of
// the LHS (slot address) AND the RHS (slot value) on the same
// captured-class array field.
TEST(LambdaCapturedArrayWriteTests, readModifyWriteCapturedArraySlot) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Probe p = new Probe(4);\n"
        "        p.log[0] = 10;\n"
        "        (int32, int32) -> void fn = (int32 idx, int32 add) -> {\n"
        "            p.log[idx] = p.log[idx] + add;\n"
        "        };\n"
        "        fn(0, 5);\n"
        "        fn(0, 7);\n"
        "        return p.log[0];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 22);
}

// Write happens inside a for-loop body. Same coverage as the
// if-branch test but for ForStatement.
TEST(LambdaCapturedArrayWriteTests, writeInsideForLoopBody) {
    auto src = std::string(kProbeWithArray) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Probe p = new Probe(6);\n"
        "        (int32) -> void fn = (int32 n) -> {\n"
        "            for (int32 i = 0; i < n; i = i + 1) {\n"
        "                p.log[p.next] = i + 1;\n"
        "                p.next = p.next + 1;\n"
        "            }\n"
        "        };\n"
        "        fn(4);\n"
        "        // p.log[0..3] = [1, 2, 3, 4]; p.next == 4.\n"
        "        return p.next * 100 + p.log[0] + p.log[1] + p.log[2] + p.log[3];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 410);
}
