// Unit 7 of the slices plan: array `Slice<T>` (slice-spec §2, §7.2).
//
// TDD — written before the grammar/codegen. `arr[a:b]` doesn't parse yet, so
// every test fails at compile (the right red). Target semantics:
//   - `arr[a:b]` yields a Slice<T> value: {store, offset, len} — three words,
//     by value, no heap object, no element copy (2.4).
//   - `s[i]` reads store[offset+i]; out-of-window is a bounds abort (2.2).
//   - `s.count()` is the window length (2.3).
//   - Sub-slicing composes in O(1) and attributes to the ROOT (2.1).
//   - Escapes follow the §4.2 table like strings: local = borrow (zero rc);
//     stored ≤256 B of payload copies; larger shares; arena always copies
//     (7.2). [D-thresh] = 256 B of element payload.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Ut {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 7.1.1 — zero-copy window over an int64 array: same elements, no allocation
// beyond the value itself; sub-slicing composes to the root; count() is the
// window length.
TEST(ArraySliceTests, arraySliceZeroCopy) {
    EXPECT_EQ(runJit(
        "int64[] arr = heap int64[64];\n"
        "int32 i = 0;\n"
        "while (i < 64) { arr[i] = (int64) (i * 3); i = i + 1; }\n"
        "int64 before = Cajeta.allocatedBytes();\n"
        "Slice<int64> s = arr[8:24];\n"                        // 16 elements
        "if (Cajeta.allocatedBytes() != before) { return -1; }\n" // no copy, no heap slice obj
        "if (s.count() != 16) { return -2; }\n"
        "if (s[0] != (int64) 24) { return -3; }\n"             // arr[8]
        "if (s[15] != (int64) 69) { return -4; }\n"            // arr[23]
        "Slice<int64> t = s[4:12];\n"                          // sub-slice: arr[12..20)
        "if (t.count() != 8) { return -5; }\n"
        "if (t[0] != (int64) 36) { return -6; }\n"             // arr[12]
        "if (Cajeta.allocatedBytes() != before) { return -7; }\n"
        "return 1;"), 1);
}

// 7.1.2 — the escape table, array edition: a small (≤256 B payload) slice
// stored past its scope copies (no stake, independent); a large one shares
// (stake, no copy).
TEST(ArraySliceTests, arraySliceEscapeResolves) {
    std::string src =
        "package test;\n"
        "public class Keep {\n"
        "    public Slice<int64> v;\n"
        "    public Keep(Slice<int64> v) {\n"
        "        this.v = v;\n"
        "    }\n"
        "}\n"
        "public final class Ut {\n"
        "    public static int32 run() {\n"
        "        int64 pop = Cajeta.sharedPopulation();\n"
        "        int64[] seed = heap int64[1];\n"
        "        Keep small = heap Keep(seed[0:0]);\n"
        "        Keep large = heap Keep(seed[0:0]);\n"
        "        {\n"
        "            int64[] arr = heap int64[512];\n"
        "            int32 i = 0;\n"
        "            while (i < 512) { arr[i] = (int64) i; i = i + 1; }\n"
        "            small.v = arr[10:26];\n"                   // 16 elems = 128 B: copy row
        "            large.v = arr[100:200];\n"                 // 100 elems = 800 B: share row
        "            if (Cajeta.sharedPopulation() < pop + 1) { return -1; }\n"
        "        }\n"                                            // arr owner drops; root pinned
        "        if (small.v.count() != 16) { return -2; }\n"
        "        if (small.v[0] != (int64) 10) { return -3; }\n"
        "        if (large.v.count() != 100) { return -4; }\n"
        "        if (large.v[99] != (int64) 199) { return -5; }\n"
        "        small.v = seed[0:0];\n"                           // detach both stakes
        "        large.v = seed[0:0];\n"
        "        if (Cajeta.sharedPopulation() != pop) { return -6; }\n" // exact retirement
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}

// 7.1.3 — an escaping slice of an arena-backed array copies at any size
// (valid after the scope; zero remaining stakes once holders detach).
TEST(ArraySliceTests, arraySliceOfArenaCopies) {
    std::string src =
        "package test;\n"
        "public class Keep {\n"
        "    public Slice<int64> v;\n"
        "    public Keep(Slice<int64> v) {\n"
        "        this.v = v;\n"
        "    }\n"
        "}\n"
        "public final class Ut {\n"
        "    public static int64 sumLocal(int32 n) {\n"
        "        int64[] tmp = heap int64[n];\n"                 // arena-eligible local
        "        int32 i = 0;\n"
        "        while (i < n) { tmp[i] = (int64) (i + 1); i = i + 1; }\n"
        "        Slice<int64> w = tmp[0:8];\n"
        "        int64 acc = 0;\n"
        "        i = 0;\n"
        "        while (i < 8) { acc = acc + w[i]; i = i + 1; }\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        if (sumLocal(32) != (int64) 36) { return -1; }\n"  // 1..8
        "        int64[] seed = heap int64[1];\n"
        "        Keep k = heap Keep(seed[0:0]);\n"
        "        int64 pop = Cajeta.sharedPopulation();\n"
        "        {\n"
        "            int64[] arr = heap int64[32];\n"
        "            int32 i = 0;\n"
        "            while (i < 32) { arr[i] = (int64) (i * 7); i = i + 1; }\n"
        "            k.v = arr[4:12];\n"                          // 64 B payload
        "        }\n"
        "        if (k.v.count() != 8) { return -2; }\n"
        "        if (k.v[0] != (int64) 28) { return -3; }\n"      // arr[4]
        "        k.v = seed[0:0];\n"                               // detach the copied root
        "        if (Cajeta.sharedPopulation() != pop) { return -4; }\n" // exact retirement
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}
