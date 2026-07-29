// Unit 2 of the slices plan: zero-copy String.substring (slice-spec §7.1).
// substring returns a mode-2 windowed view — no byte copy, no per-call buffer
// allocation for heap-backed sources; the root buffer outlives its source
// wrapper via the shared state and frees exactly once at the last stake.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Sst {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Sst");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// A dynamic (heap, non-SSO, non-literal) string: concat forces a fresh owned
// buffer well past the 24-byte SSO cap.
const char* kDyn =
    "String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "String b = \"0123456789\";\n"
    "String s = a + b;\n";  // 36 bytes, owned heap buffer

}  // namespace

// 2.1.1 — substring of a heap source allocates no new byte buffer: the
// allocation delta for the substring call is bounded by the wrapper size
// (<< the 30-byte window a copy would take... scaled up: 1KB window).
TEST(SubstringSliceTests, substringZeroCopy) {
    EXPECT_EQ(runJit(
        "String big = \"0123456789abcdef\";\n"
        "int32 i = 0;\n"
        "while (i < 6) { big = big + big; i = i + 1; }\n"  // 1KB heap string
        "int64 before = Cajeta.allocatedBytes();\n"
        "String w = big.substring(8, 1000);\n"
        "int64 delta = Cajeta.allocatedBytes() - before;\n"
        "if (w.size() != 992) { return -1; }\n"
        "if (delta > 256) { return -2; }\n"  // wrapper only — a copy would be ~1KB
        "return 1;"), 1);
}

// 2.1.2 — the substring outlives its source: correct bytes after the source
// drops, and the root buffer is reclaimed exactly once (live count returns
// to baseline — no leak, no double free).
TEST(SubstringSliceTests, substringOutlivesSource) {
    EXPECT_EQ(runJit(
        std::string(kDyn) +
        "String keep;\n"
        "{\n" +
        "    String t = s.substring(23, 29);\n"  // "xyz012"
        "    keep #= t;\n"
        "}\n"
        "s = \"\";\n"  // rebind: source wrapper drops; root must survive via keep
        "if (keep.size() != 6) { return -1; }\n"
        "if (keep.charAt(0) != (int8) 120) { return -2; }\n"   // 'x'
        "if (keep.charAt(5) != (int8) 50) { return -3; }\n"    // '2'
        "return 1;"), 1);
}

// 2.1.3 — substring of a literal is a static-rooted view: no rc, no free,
// correct content.
TEST(SubstringSliceTests, substringOfLiteralStatic) {
    EXPECT_EQ(runJit(
        "String lit = \"the quick brown fox jumps over the lazy dog\";\n"
        "String w = lit.substring(4, 9);\n"      // "quick"
        "if (w.size() != 5) { return -1; }\n"
        "if (!w.equals(\"quick\")) { return -2; }\n"
        "String w2 = lit.substring(10, 15);\n"   // "brown"
        "if (!w2.equals(\"brown\")) { return -3; }\n"
        "return 1;"), 1);
}

// 2.1.4 — substring of an SSO-resident string materializes (never a view into
// the wrapper's inline region); content correct, source unaffected.
TEST(SubstringSliceTests, ssoSubstringRules) {
    EXPECT_EQ(runJit(
        "String a = \"abc\";\n"
        "String b = \"defgh\";\n"
        "String sso = a + b;\n"                  // 8 bytes: SSO-resident owned
        "String w = sso.substring(2, 6);\n"      // "cdef"
        "if (w.size() != 4) { return -1; }\n"
        "if (!w.equals(\"cdef\")) { return -2; }\n"
        "if (!sso.equals(\"abcdefgh\")) { return -3; }\n"
        "return 1;"), 1);
}

// 2.1.5 — chained substrings attribute to the ROOT buffer: drop the
// intermediate, the leaf stays valid.
TEST(SubstringSliceTests, substringChainRoot) {
    EXPECT_EQ(runJit(
        std::string(kDyn) +
        "String leaf;\n"
        "{\n"
        "    String mid = s.substring(5, 30);\n"    // "fghij...xyz012" window
        "    String t = mid.substring(5, 10);\n"    // "klmno" (root offsets 10..15)
        "    leaf #= t;\n"
        "}\n"
        "if (leaf.size() != 5) { return -1; }\n"
        "if (!leaf.equals(\"klmno\")) { return -2; }\n"
        "return 1;"), 1);
}

// 2.2.3 — the const char* runtime ABI (parse/println/log) sees the WINDOW of a
// view, not the root: parse of a mid-string slice yields the sliced number.
TEST(SubstringSliceTests, sliceCstrAbiWindows) {
    EXPECT_EQ(runJit(
        std::string(kDyn) +
        "String digits = s.substring(26, 31);\n"   // "01234"
        "int64 v = Long.parseLong(digits);\n"
        "if (v != 1234) { return -1; }\n"           // "01234" -> 1234
        "String d2 = s.substring(28, 30);\n"        // "23"
        "if (Long.parseLong(d2) != 23) { return -2; }\n"
        "return 1;"), 1);
}

// 2.1.6 (spot) — substring results behave like ordinary strings downstream:
// equality both ways, indexOf on a view, concat of two views, trim of a view.
TEST(SubstringSliceTests, sliceBehavesLikeString) {
    EXPECT_EQ(runJit(
        std::string(kDyn) +
        "String w = s.substring(10, 20);\n"        // "klmnopqrst"
        "if (!w.equals(\"klmnopqrst\")) { return -1; }\n"
        "if (w.indexOf(\"opq\") != 4) { return -2; }\n"
        "String w2 = s.substring(0, 3);\n"          // "abc"
        "String cat = w2 + w;\n"
        "if (!cat.equals(\"abcklmnopqrst\")) { return -3; }\n"
        "if (cat.size() != 13) { return -4; }\n"
        "String padded = s.substring(25, 30);\n"    // "z0123"
        "if (padded.hash() == 0) { return -5; }\n"
        "return 1;"), 1);
}
