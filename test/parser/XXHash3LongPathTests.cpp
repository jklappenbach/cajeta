// XXHash3's in-language long path (inputs over 240 bytes) against the native
// reference. Its scalar int64 multiplies trapped on signed overflow, so every
// hash of a long input ended in SIGILL (found 2026-10-13 by the calibration
// record codec). A uint64 multiply wraps, which is what XXH3 means.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.hash.Hash;\n"
    "import cajeta.hash.XXHash3;\n"
    "public final class D {\n"
    "    static #int8[] bytes(int32 n) {\n"
    "        int8[] b = heap int8[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { b[i] = (int8) (i * 131 + 7); i = i + 1; }\n"
    "        return b;\n"
    "    }\n"
    // every length 241..2100 and three seeds: the long path equals the native reference
    "    public static int64 longPathMatchesNative() {\n"
    "        int32 n = 241;\n"
    "        while (n <= 2100) {\n"
    "            int8[] b #= D.bytes(n);\n"
    "            int64[] seeds = heap int64[3];\n"
    "            seeds[0] = 0L; seeds[1] = 0x9E3779B185EBCA87L; seeds[2] = 0L - 1L;\n"
    "            int32 s = 0;\n"
    "            while (s < 3) {\n"
    "                int64 want = XXHash3.hashSeeded(b, (int64) n, seeds[s]);\n"
    "                int64 got = XXHash3.hashLong(b, (int64) n, seeds[s]);\n"
    "                if (got != want) { return (int64) n * 10L + (int64) s; }\n"
    "                s = s + 1;\n"
    "            }\n"
    "            n = n + 1;\n"
    "        }\n"
    "        return 0L;\n"
    "    }\n"
    // the public one-shot routes long inputs to that path under the process seed
    "    public static int64 hashOfALongInput() {\n"
    "        int8[] b #= D.bytes(4096);\n"
    "        int64 got = XXHash3.hash(b, 4096L);\n"
    "        int64 want = XXHash3.hashSeeded(b, 4096L, Hash.processSeed());\n"
    "        if (got != want) { return 1L; }\n"
    "        return 0L;\n"
    "    }\n"
    "}\n";

int64_t run(const char* fn) {
    auto jit = CajetaJit::compile(kSource, "test.D");
    if (!jit) return -100;
    auto f = jit->lookup<int64_t (*)()>(fn);
    return f ? f() : -101;
}

} // namespace

TEST(XXHash3LongPath, everyLongLengthMatchesTheNativeReference) {
    EXPECT_EQ(run("longPathMatchesNative"), 0) << "first mismatch as length * 10 + seed index";
}

TEST(XXHash3LongPath, theOneShotHashOfALongInputDoesNotTrap) {
    EXPECT_EQ(run("hashOfALongInput"), 0);
}
