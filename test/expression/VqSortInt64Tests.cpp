//
// Concrete Sort.sort(int64[], int32) — the AVX-512 vqsort fast path (Blacher
// in-place vpcompressq partition). A bare `Sort.sort(arr, n)` resolves to this
// over the generic `sort<T>` (concrete beats template). These exercise the
// vectorized partition across input patterns and sizes straddling the 128-
// element vector threshold and the 8-lane peel, verifying full order AND
// multiset preservation (sum check).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<int64_t (*)()>("run");
    return f ? f() : -1;
}

// Builds a class that fills n int64 by `expr` (in terms of loop var i and state
// x), sorts via the concrete overload, and returns 0 iff sorted + sum-preserved.
std::string prog(int n, const char* fill) {
    return std::string(
        "package test;\n"
        "import cajeta.collection.Sort;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32 n = ") + std::to_string(n) + ";\n"
        "        int64[] a = heap int64[n];\n"
        "        int64 x = 999;\n"
        "        int64 sb = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            int64 v = 0;\n" + fill +
        "            a[i] = v; sb = sb + v; i = i + 1;\n"
        "        }\n"
        "        Sort.sort(a, n);\n"
        "        int64 sa = 0; int32 bad = 0; i = 0;\n"
        "        while (i < n) {\n"
        "            sa = sa + a[i];\n"
        "            if (i > 0 && a[i] < a[i-1]) { bad = bad + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (sa != sb) { return -1; }\n"
        "        return (int64) bad;\n"
        "    }\n}\n";
}

const char* RANDOM = "            x = (x * 1103515245 + 12345) & 0x7FFFFFFF; v = x;\n";
const char* DUPS   = "            v = (int64)(i % 16);\n";
const char* REVERSE= "            v = (int64)(n - 1 - i);\n";
const char* EQUAL  = "            v = 7;\n";
const char* ASCEND = "            v = (int64) i;\n";
// Ascending for the first half, then random — exercises the partial-run case of
// pdqFindRun (run shorter than n, must fall through to the real sort).
const char* PARTIAL= "            if (i < n/2) { v = (int64) i; } else { x = (x * 1103515245 + 12345) & 0x7FFFFFFF; v = x; }\n";

} // namespace

// benchmark-gap-sweep Unit 4 — already-ascending input: pdqFindRun's single-scan
// fast path returns runLen==n and the sort early-outs, leaving order intact. This
// guards the register-carried-prev rewrite of pdqFindRun.
TEST(VqSortInt64Tests, ascendingFastPath) {
    EXPECT_EQ(runI64(prog(4096, ASCEND)), 0);
}

// Partial ascending run then random tail — pdqFindRun returns runLen < n and the
// full sort must still produce a fully ordered, multiset-preserving result.
TEST(VqSortInt64Tests, partialRun) {
    EXPECT_EQ(runI64(prog(4096, PARTIAL)), 0);
}

// Large random input — the main vectorized-partition path (n >> 128).
TEST(VqSortInt64Tests, randomLarge) {
    EXPECT_EQ(runI64(prog(4096, RANDOM)), 0);
}

// Low-cardinality (dups) — exercises the ancestor `<=` path interleaved with
// the vectorized `<` partition; the workload where vqsort wins biggest.
TEST(VqSortInt64Tests, dups) {
    EXPECT_EQ(runI64(prog(4096, DUPS)), 0);
}

// Reverse-sorted and all-equal — adversarial for partition balance.
TEST(VqSortInt64Tests, reverse) {
    EXPECT_EQ(runI64(prog(2000, REVERSE)), 0);
}

TEST(VqSortInt64Tests, allEqual) {
    EXPECT_EQ(runI64(prog(2000, EQUAL)), 0);
}

// Sizes straddling the 128 threshold and the 8-lane peel remainder — all in a
// single JIT compile (looping internally) so the test stays well under the
// harness timeout. Returns the first failing size (with a 1/2 order/sum tag),
// or 0 if every size sorts correctly with its multiset preserved.
TEST(VqSortInt64Tests, boundarySizes) {
    const char* src =
        "package test;\n"
        "import cajeta.collection.Sort;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] sizes = [ 120,121,122,123,124,125,126,127,128,129,\n"
        "            130,131,132,133,134,135,136,255,256,257,300 ];\n"
        "        int32 ns = 21; int32 si = 0;\n"
        "        while (si < ns) {\n"
        "            int32 n = sizes[si];\n"
        "            int64[] a = heap int64[n];\n"
        "            int64 x = 12345; int64 sb = 0; int32 i = 0;\n"
        "            while (i < n) {\n"
        "                x = (x * 1103515245 + 12345) & 0x7FFFFFFF;\n"
        "                a[i] = x; sb = sb + x; i = i + 1;\n"
        "            }\n"
        "            Sort.sort(a, n);\n"
        "            int64 sa = 0; int32 bad = 0; i = 0;\n"
        "            while (i < n) {\n"
        "                sa = sa + a[i];\n"
        "                if (i > 0 && a[i] < a[i-1]) { bad = bad + 1; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (bad > 0) { return (int64) n * 10 + 1; }\n"
        "            if (sa != sb) { return (int64) n * 10 + 2; }\n"
        "            si = si + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n}\n";
    EXPECT_EQ(runI64(src), 0);
}
