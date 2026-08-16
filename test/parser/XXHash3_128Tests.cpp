// XXH3-128 tests. Pin the native small-input path to the official
// XXH3_128bits_withSeed reference (vendored xxHash 0.8.3).
//
// SCOPE: these JIT tests cover only the native path (inputs <= 240 B). The
// pure-Cajeta AVX-512 SIMD long path (hashLong128Into, inputs > 240 B) cannot
// run under the JIT — the JIT TargetMachine does not enable AVX-512, so the
// Vector<int64,8> lowering traps (SIGILL), exactly as the existing 64-bit
// hashLong does (which is why XXHash3Bench/XXHash3Tests never JIT it). The SIMD
// long path's bit-for-bit agreement with the native reference is verified AOT in
// samples/profile XXHash3_128Bench.checkResult (pinned to the same constants).
//
// Reference vectors generated with the vendored xxhash.h:
//   XXH3_128bits_withSeed(data, len, seed) -> {low64, high64}

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// JIT a function returning `lane` (0=low64, 1=high64) of
// XXHash3.hash128Seeded over a `count`-byte buffer filled by `fillExpr` (a
// function of loop index `i`) under `seed`. Inputs are kept <= 240 B so this
// stays on the native path the JIT can execute.
int64_t lane128(const std::string& fillExpr, size_t count, uint64_t seed, int lane) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 n = " + std::to_string(count) + "L;\n"
        "        int64 seed = " + std::to_string((int64_t) seed) + "L;\n"
        "        int8[] data = heap int8[n];\n"
        "        int64 i = 0L;\n"
        "        while (i < n) { data[i] = (int8) (" + fillExpr + "); i = i + 1L; }\n"
        "        int8[] out = heap int8[16];\n"
        "        XXHash3.hash128Into(data, n, seed, out);\n"
        "        return Cajeta.loadU64(out, " + std::to_string(lane * 8) + "L);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

int64_t low(const std::string& fill, size_t n, uint64_t seed)  { return lane128(fill, n, seed, 0); }
int64_t high(const std::string& fill, size_t n, uint64_t seed) { return lane128(fill, n, seed, 1); }

} // namespace

TEST(XXHash3_128Tests, emptyMatchesReference) {
    EXPECT_EQ((uint64_t) low("0", 0, 0),  0x6001c324468d497fULL);
    EXPECT_EQ((uint64_t) high("0", 0, 0), 0x99aa06d3014798d8ULL);
}

TEST(XXHash3_128Tests, abcMatchesReference) {
    const char* fill = "97L + i";  // i in {0,1,2} -> 'a','b','c'
    EXPECT_EQ((uint64_t) low(fill, 3, 0),  0x78af5f94892f3950ULL);
    EXPECT_EQ((uint64_t) high(fill, 3, 0), 0x06b05ab6733a6185ULL);
}

// 240 bytes (i & 0xFF) — the largest input still on the native path (the SIMD
// threshold is > 240). Exercises the native bridge across a full mid-size buffer.
TEST(XXHash3_128Tests, nativePathBoundary240) {
    const char* fill = "i & 0xFFL";
    EXPECT_EQ((uint64_t) low(fill, 240, 0),  0xc92b68e16f83bbb6ULL);
    EXPECT_EQ((uint64_t) high(fill, 240, 0), 0x65b5be86da5540e7ULL);
}

// hash128Hex canonical form (big-endian high64 then low64), seeded so it's
// deterministic. "abc" canonical = 06b05ab6733a618578af5f94892f3950.
TEST(XXHash3_128Tests, hexCanonicalForm) {
    auto src =
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        int8[] data = heap int8[3];\n"
        "        data[0L] = (int8) 97; data[1L] = (int8) 98; data[2L] = (int8) 99;\n"
        "        String s #= XXHash3.hash128HexSeeded(data, 3L, 0L);\n"
        "        int8[] out #= s.toBytes();\n"
        "        return #out;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    ASSERT_NE(hdr, nullptr);
    int64_t count = *(int64_t*) hdr;
    std::string out((const char*) hdr + 8, (size_t) count);
    EXPECT_EQ(out, "06b05ab6733a618578af5f94892f3950");
}
