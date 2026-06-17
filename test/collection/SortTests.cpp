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
TEST(SortTests, sortAscending) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a = { 5, 3, 8, 1, 9, 2, 7 };\n"
        "        Sort.sort<int32>(a, 7);\n"
        "        int32 i = 1;\n"
        "        while (i < 7) {\n"
        "            if (a[i] < a[i - 1]) { return 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return a[0] * 100 + a[6];\n"   // 1*100 + 9 = 109
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 109);
}

// Adversarial inputs (reverse-sorted, large n) stay correct and don't blow the
// bounded range stack — exercises the recurse-smaller/loop-larger discipline.
TEST(SortTests, sortReverseSortedLargeN) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 n = 500;\n"
        "        int32[] a = heap int32[500];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] = n - i; i = i + 1; }\n"   // 500..1
        "        Sort.sort<int32>(a, n);\n"
        "        int32 k = 0;\n"
        "        while (k < n) {\n"
        "            if (a[k] != k + 1) { return -1; }\n"        // expect 1..500
        "            k = k + 1;\n"
        "        }\n"
        "        return a[0] + a[499];\n"                        // 1 + 500 = 501
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 501);
}

// Stable mergesort sorts ascending.
TEST(SortTests, sortStableAscending) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a = { 5, 3, 8, 1, 9, 2, 7, 4, 6 };\n"
        "        Sort.sortStable<int32>(a, 9);\n"
        "        int32 i = 1;\n"
        "        while (i < 9) {\n"
        "            if (a[i] < a[i - 1]) { return 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return a[0] * 10 + a[8];\n"   // 1*10 + 9 = 19
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 19);
}

// Stability is observable: pack values as key*256 + tag, sort by KEY only with
// a comparator; within an equal-key group the tags must stay in input order.
TEST(SortTests, stableSortPreservesEqualKeyOrder) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // keys: 2,1,2,1,2,1  tags ascending per insertion: enc = key*256 + tag
        "        int32[] a = { 512, 256, 513, 257, 514, 258 };\n"
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
TEST(SortTests, comparatorDescending) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a = { 5, 3, 8, 1, 9 };\n"
        "        Sort.sort<int32>(a, 5, (x, y) -> {\n"
        "            if (x > y) { return -1; }\n"
        "            if (x < y) { return 1; }\n"
        "            return 0;\n"
        "        });\n"
        "        return a[0] * 10 + a[4];\n"   // 9*10 + 1 = 91
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 91);
}

// lowerBound / upperBound / binarySearch over a sorted array with duplicates.
TEST(SortTests, searchBounds) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a = { 1, 2, 2, 2, 5, 8 };\n"
        "        int32 lb = Sort.lowerBound<int32>(a, 6, 2);\n"    // 1
        "        int32 ub = Sort.upperBound<int32>(a, 6, 2);\n"    // 4
        "        int32 bs = Sort.binarySearch<int32>(a, 6, 5);\n"  // 4
        "        int32 miss = Sort.binarySearch<int32>(a, 6, 7);\n" // -1
        "        return lb * 1000 + ub * 100 + bs * 10 + (miss + 1);\n" // 1440
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1440);
}

// Sorting an ArrayList<T> in place via the instance method (sorts the backing).
TEST(SortTests, sortArrayList) {
    std::string src = std::string(PRE) +
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(5);\n"
        "        xs.add(3);\n"
        "        xs.add(8);\n"
        "        xs.add(1);\n"
        "        xs.sort();\n"
        "        return xs.get(0) * 1000 + xs.get(1) * 100 + xs.get(2) * 10 + xs.get(3);\n" // 1358
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1358);
}

// Generic over a floating dtype (float32) — natural order via `<`.
TEST(SortTests, sortFloat32) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] f = { 3.5f, 1.2f, 2.8f };\n"
        "        Sort.sort<float32>(f, 3);\n"
        "        if (f[0] > f[1]) { return 0; }\n"
        "        if (f[1] > f[2]) { return 0; }\n"
        "        return (int32)(f[0] * 10.0f) + (int32)(f[2] * 10.0f);\n" // 12 + 35 = 47
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 47);
}
