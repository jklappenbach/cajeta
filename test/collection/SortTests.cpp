//
// SortTests — the general host sorting facility (cajeta.collection.Sort):
// sorting-spec.md §4a (the host sort) + §4c (the shared comparator/search seam).
// Comparator-based core with natural-order (`<`/`>`) wrappers; quicksort
// (unstable) + bottom-up mergesort (stable); lowerBound/upperBound/binarySearch.
//

#include <gtest/gtest.h>

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

const char* PRE = "package test;\nimport cajeta.collection.Sort;\n";

} // namespace

// Natural-order quicksort sorts ascending; verify order + endpoints (1..9).

// Adversarial inputs (reverse-sorted, large n) stay correct and don't blow the
// bounded range stack — exercises the recurse-smaller/loop-larger discipline.

// Stable mergesort sorts ascending.

// Stability is observable: pack values as key*256 + tag, sort by KEY only with
// a comparator; within an equal-key group the tags must stay in input order.
TEST(SortTests, stableSortPreservesEqualKeyOrder) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // keys: 2,1,2,1,2,1  tags ascending per insertion: enc = key*256 + tag
        "        int32[] a = [ 512, 256, 513, 257, 514, 258 ];\n"
        "        Sort.sortStable<int32>(a, 6, (x, y) -> {\n"
        "            int32 kx = x / 256;\n"
        "            int32 ky = y / 256;\n"
        "            if (kx < ky) { return -1; }\n"
        "            if (kx > ky) { return 1; }\n"
        "            return 0;\n"
        "        });\n"
        // expect key=1 group first (tags 0,1,2 -> 256,257,258), then key=2 (512,513,514)
        "        if (a[0] != 256) { return -1; }\n"
        "        if (a[1] != 257) { return -2; }\n"
        "        if (a[2] != 258) { return -3; }\n"
        "        if (a[3] != 512) { return -4; }\n"
        "        if (a[4] != 513) { return -5; }\n"
        "        if (a[5] != 514) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// A custom comparator sorts descending (the seam, supplied at the call site).

// lowerBound / upperBound / binarySearch over a sorted array with duplicates.

// Sorting an ArrayList<T> in place via the instance method (sorts the backing).

// Generic over a floating dtype (float32) — natural order via `<`.
