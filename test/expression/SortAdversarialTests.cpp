//
// Sort.sort correctness on adversarial int64 patterns (sort-adversarial plan
// Unit 1; spec §2). Verifies the unstable quicksort (middle pivot + loop-smaller
// / push-larger range stack) produces a non-decreasing permutation for
// ascending / descending / dups / random / organ-pipe inputs.
//
// Sizes are kept JIT-safe (n=2000 balanced, n=128 organ-pipe): at -O0 the JIT
// does NOT promote per-iteration loop-local allocas to registers, so a
// comparator-heavy sort (O(n log n) / O(n^2) cmp calls) accumulates stack
// allocas and overflows the native stack at large n. That is a SEPARATE O0
// codegen issue (release -O3 sorts n=50000 of every pattern fine — see the
// `sort-int64` benchmark, all variants check=true). Scale is verified there;
// these tests verify CORRECTNESS.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kProgram =
    "package test;\n"
    "import cajeta.collection.Sort;\n"
    "public final class A {\n"
    "    static int32 check(int64[] w, int32 n, int64 expectSum) {\n"
    "        Sort.sort<int64>(w, n);\n"
    "        int64 s = 0;\n"
    "        int32 j = 0;\n"
    "        while (j < n) { s = s + w[j]; j = j + 1; }\n"
    "        if (s != expectSum) { return 2; }\n"            // lost/duplicated element
    "        int32 k = 1;\n"
    "        while (k < n) { if (w[k] < w[k - 1]) { return 3; } k = k + 1; }\n" // not sorted
    "        return 0;\n"
    "    }\n"
    "    public static int32 ascending() {\n"
    "        int32 n = 2000; int64[] w = heap int64[n]; int64 sum = 0; int32 i = 0;\n"
    "        while (i < n) { w[i] = (int64) i; sum = sum + (int64) i; i = i + 1; }\n"
    "        return check(w, n, sum);\n"
    "    }\n"
    "    public static int32 descending() {\n"
    "        int32 n = 2000; int64[] w = heap int64[n]; int64 sum = 0; int32 i = 0;\n"
    "        while (i < n) { int64 v = (int64)(n - 1 - i); w[i] = v; sum = sum + v; i = i + 1; }\n"
    "        return check(w, n, sum);\n"
    "    }\n"
    "    public static int32 dups() {\n"
    "        int32 n = 2000; int64[] w = heap int64[n]; int64 sum = 0; int32 i = 0;\n"
    "        while (i < n) { int64 v = (int64)(i % 100); w[i] = v; sum = sum + v; i = i + 1; }\n"
    "        return check(w, n, sum);\n"
    "    }\n"
    "    public static int32 random() {\n"
    "        int32 n = 2000; int64[] w = heap int64[n]; int64 sum = 0; int32 i = 0;\n"
    "        int64 x = 88172645;\n"
    "        while (i < n) {\n"
    "            x = (x * 1103515245 + 12345) & 0x7FFFFFFF;\n"
    "            w[i] = x; sum = sum + x; i = i + 1;\n"
    "        }\n"
    "        return check(w, n, sum);\n"
    "    }\n"
    // Organ-pipe (0..peak..0): a maximally-imbalanced shape for a single-element
    // pivot. Small n keeps the O(n^2) cmp count under the O0 stack-alloca limit.
    "    public static int32 organpipe() {\n"
    "        int32 n = 128; int64[] w = heap int64[n]; int64 sum = 0; int32 i = 0; int32 half = n / 2;\n"
    "        while (i < n) {\n"
    "            int64 v; if (i < half) { v = (int64) i; } else { v = (int64)(n - 1 - i); }\n"
    "            w[i] = v; sum = sum + v; i = i + 1;\n"
    "        }\n"
    "        return check(w, n, sum);\n"
    "    }\n"
    "}\n";

int32_t call(CajetaJit* jit, const char* sym) {
    auto fn = jit->lookup<int32_t (*)()>(sym);
    EXPECT_NE(fn, nullptr) << sym;
    return fn ? fn() : -1;
}

} // namespace

// 2.1.1-2.1.5 — every pattern sorts to a non-decreasing permutation.
TEST(SortAdversarialTests, AllPatternsSortCorrectly) {
    auto jit = CajetaJit::compile(kProgram, "test.A");
    EXPECT_EQ(call(jit.get(), "ascending"), 0);
    EXPECT_EQ(call(jit.get(), "descending"), 0);
    EXPECT_EQ(call(jit.get(), "dups"), 0);
    EXPECT_EQ(call(jit.get(), "random"), 0);
    EXPECT_EQ(call(jit.get(), "organpipe"), 0);
}
