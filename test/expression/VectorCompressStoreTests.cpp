//
// Vector<T,N>.compressStore(dst, mask) — the AVX-512 vpcompress* partition
// primitive (the building block of a vectorized quicksort / x86-simd-sort).
// `v.compressStore(out[w], mask)` packs the lanes of `v` whose mask bit is set
// into contiguous memory at &out[w], low lane first, and returns popcount(mask)
// — how many elements were written. Lowers to @llvm.masked.compressstore;
// on a native AVX-512 build it becomes a single `vpcompressq`.
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

const char* head =
    "package test;\n"
    "public final class D {\n"
    "    public static int64 run() {\n"
    "        int64[] a = [ 5, 1, 8, 2, 7, 3, 6, 4 ];\n"
    "        int64[] out = [ 0, 0, 0, 0, 0, 0, 0, 0 ];\n"
    "        Vector<int64,8> v = a.vload<8>(0);\n";
const char* tail = "    }\n}\n";

} // namespace

// Partition-by-pivot: mask (v < 5) selects lanes 1,3,5,7 -> values 1,2,3,4.
// compressStore packs them low-lane-first, returns count 4. Encode count and
// the first four compacted values: 4*10000 + 1*1000 + 2*100 + 3*10 + 4 = 41234.
TEST(VectorCompressStoreTests, ltPivotPacksAndCounts) {
    std::string src = std::string(head) +
        "        int32 c = v.compressStore(out[0], (v < 5));\n"
        "        return (int64) c * 10000 + out[0] * 1000 + out[1] * 100\n"
        "            + out[2] * 10 + out[3];\n" + tail;
    EXPECT_EQ(runI64(src), 41234);
}

// All lanes pass (v < 100): count 8, original order preserved {5,1,8,...}.
// 8*1000 + 5*100 + 1*10 + 8 = 8518.
TEST(VectorCompressStoreTests, allTrueKeepsOrder) {
    std::string src = std::string(head) +
        "        int32 c = v.compressStore(out[0], (v < 100));\n"
        "        return (int64) c * 1000 + out[0] * 100 + out[1] * 10 + out[2];\n"
        + tail;
    EXPECT_EQ(runI64(src), 8518);
}

// No lane passes (v < 0): count 0, nothing written (out stays zero).
TEST(VectorCompressStoreTests, allFalseWritesNothing) {
    std::string src = std::string(head) +
        "        int32 c = v.compressStore(out[0], (v < 0));\n"
        "        return (int64) c * 10 + out[0];\n" + tail;
    EXPECT_EQ(runI64(src), 0);
}

// Non-zero destination offset: pack (v < 5) -> {1,2,3,4} starting at out[2],
// exercising the &out[offset] element-pointer addressing. out[2..6)={1,2,3,4}
// and out[0] stays 0: 4*100000 + 1*1000 + 2*100 + 3*10 + 4 = 401234.
TEST(VectorCompressStoreTests, writesAtOffset) {
    std::string src = std::string(head) +
        "        int32 c = v.compressStore(out[2], (v < 5));\n"
        "        return (int64) c * 100000 + out[2] * 1000 + out[3] * 100\n"
        "            + out[4] * 10 + out[5];\n" + tail;
    EXPECT_EQ(runI64(src), 401234);
}
