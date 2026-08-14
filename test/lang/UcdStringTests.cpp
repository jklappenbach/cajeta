//
// UcdStringTests — stdlib-completion plan Unit 7, the cajeta.lang surface
// (spec §7): String.nfc/nfd/nfkc/nfkd + isNfc/isNfd, caseFold (distinct
// from toLowerCase), stripDefaultIgnorables (separable from
// normalization), EncodingException on malformed UTF-8, and the Ucd
// property class for text-shaping. The exhaustive conformance pass lives
// in test/ucd/UcdConformanceTests.cpp against the same compiled-in
// Unicode 16.0.0 tables; these pin the String-level contracts.
//
// Raw UTF-8 bytes are embedded directly in the JIT source literals.
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

const char* PRE = "package test;\n";

} // namespace

// 7.1.2 — all four forms available and DISTINCT where they differ:
// U+1E9B U+0323 (long s with dot above + dot below) is the classic
// four-way case: NFC keeps it, NFD decomposes to U+017F+marks, NFKC
// composes to U+1E69, NFKD decomposes to s+marks.
TEST(UcdStringTests, fourFormsDistinct) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String x = \"\xE1\xBA\x9B\xCC\xA3\";\n"
        "        String nfc = x.nfc();\n"
        "        String nfd = x.nfd();\n"
        "        String nfkc = x.nfkc();\n"
        "        String nfkd = x.nfkd();\n"
        "        if (!nfc.equals(\"\xE1\xBA\x9B\xCC\xA3\")) { return -1; }\n"
        "        if (!nfd.equals(\"\xC5\xBF\xCC\xA3\xCC\x87\")) { return -2; }\n"
        "        if (!nfkc.equals(\"\xE1\xB9\xA9\")) { return -3; }\n"
        "        if (!nfkd.equals(\"s\xCC\xA3\xCC\x87\")) { return -4; }\n"
        "        if (nfc.equals(nfd)) { return -5; }\n"
        "        if (nfkc.equals(nfkd)) { return -6; }\n"
        "        if (nfc.equals(nfkc)) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.3 — strings differing only in normalization form compare equal
// after normalization (§9.8): composed é vs e + combining acute.
TEST(UcdStringTests, crossFormEqualityAfterNormalization) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String composed = \"caf\xC3\xA9\";\n"
        "        String decomposed = \"cafe\xCC\x81\";\n"
        "        if (composed.equals(decomposed)) { return -1; }\n"
        "        if (!composed.nfc().equals(decomposed.nfc())) { return -2; }\n"
        "        if (!composed.nfd().equals(decomposed.nfd())) { return -3; }\n"
        "        if (!composed.isNfc()) { return -4; }\n"
        "        if (decomposed.isNfc()) { return -5; }\n"
        "        if (!decomposed.isNfd()) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.4 — FULL case folding differs from toLowerCase where it must:
// German ß folds to "ss"; dotted capital İ folds to i + combining dot.
TEST(UcdStringTests, caseFoldDiffersFromLowercase) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String sharp = \"Stra\xC3\x9F""e\";\n"
        "        if (!sharp.caseFold().equals(\"strasse\")) { return -1; }\n"
        "        if (!sharp.caseFold().equals(\"STRASSE\".caseFold())) { return -2; }\n"
        "        if (sharp.toLowerCase().equals(\"strasse\")) { return -3; }\n"
        "        String dottedI = \"\xC4\xB0\";\n"
        "        if (!dottedI.caseFold().equals(\"i\xCC\x87\")) { return -4; }\n"
        "        if (dottedI.toLowerCase().equals(\"i\xCC\x87\")) { return -5; }\n"
        // dotless i folds to itself
        "        String dotless = \"\xC4\xB1\";\n"
        "        if (!dotless.caseFold().equals(dotless)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.5 — normalization is idempotent and cheap on normalized input (the
// no-copy fast path is structural: the native returns NULL and the
// .cajeta side hands back `this`; equality is what's observable here).
TEST(UcdStringTests, idempotentAndStableOnNormalizedInput) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"caf\xC3\xA9 strasse 123\";\n"
        "        if (!s.isNfc()) { return -1; }\n"
        "        if (!s.nfc().equals(s)) { return -2; }\n"
        "        String d = s.nfd();\n"
        "        if (!d.nfd().equals(d)) { return -3; }\n"
        "        if (!d.nfc().equals(s)) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.6 — default-ignorable stripping is separable from normalization:
// stripping removes the soft hyphen without normalizing; normalizing
// keeps the soft hyphen.
TEST(UcdStringTests, stripSeparableFromNormalization) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String shy = \"co\xC2\xADop\";\n"
        "        if (!shy.stripDefaultIgnorables().equals(\"coop\")) { return -1; }\n"
        "        if (!shy.nfc().equals(shy)) { return -2; }\n"        // nfc keeps SHY
        // stripping does not normalize: decomposed stays decomposed
        "        String mixed = \"e\xCC\x81\xC2\xAD\";\n"
        "        String stripped = mixed.stripDefaultIgnorables();\n"
        "        if (!stripped.equals(\"e\xCC\x81\")) { return -3; }\n"
        "        if (stripped.equals(\"\xC3\xA9\")) { return -4; }\n"
        "        String clean = \"plain\";\n"
        "        if (!clean.stripDefaultIgnorables().equals(clean)) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.7 — malformed UTF-8 throws EncodingException, never garbage.
TEST(UcdStringTests, malformedUtf8Throws) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] bad = heap int8[2];\n"
        "        bad[0] = (int8) -61;\n"        // 0xC3 lead byte
        "        bad[1] = (int8) 40;\n"         // '(' — not a continuation
        "        String s = heap String(#bad, 2);\n"
        "        int32 caught = 0;\n"
        "        try {\n"
        "            String x = s.nfc();\n"
        "            if (x.equals(\"never\")) { return 99; }\n"
        "        } catch (EncodingException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            String y = s.caseFold();\n"
        "            if (y.equals(\"never\")) { return 99; }\n"
        "        } catch (EncodingException ex) { caught = caught + 1; }\n"
        "        if (caught != 2) { return -caught; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.2.8 — the Ucd property surface for cajeta-text-shaping §13.2, from
// cajeta code: combining class, script, joining type, bidi, ignorable.
TEST(UcdStringTests, ucdPropertySurface) {
    std::string src = std::string(PRE) +
        "import cajeta.lang.Ucd;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        if (Ucd.combiningClass(769) != 230) { return -1; }\n"   // U+0301
        "        if (Ucd.combiningClass(97) != 0) { return -2; }\n"      // 'a'
        "        if (Ucd.joiningType(1575) != Ucd.JOIN_R) { return -3; }\n"  // alef
        "        if (Ucd.joiningType(1576) != Ucd.JOIN_D) { return -4; }\n"  // beh
        "        if (Ucd.joiningType(1600) != Ucd.JOIN_C) { return -5; }\n"  // tatweel
        "        if (Ucd.joiningType(769) != Ucd.JOIN_T) { return -6; }\n"   // mark
        "        if (Ucd.isDefaultIgnorable(173) != 1) { return -7; }\n"     // SHY
        "        if (Ucd.isDefaultIgnorable(120) != 0) { return -8; }\n"     // 'x'
        "        if (Ucd.script(97) != Ucd.script(122)) { return -9; }\n"    // a..z same
        "        if (Ucd.script(97) == Ucd.script(1575)) { return -10; }\n"  // != Arabic
        "        if (Ucd.bidiClass(97) == Ucd.bidiClass(1575)) { return -11; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// General category + White_Space (cajeta-llama U12: \p{L} / \p{N} / \s
// classification for tokenizer pre-splits). Category ordinals are the
// fixed canonical order (GC_CN=0, GC_LU=1, ... GC_CO=29); White_Space is
// the PropList property, not derivable from the category.
TEST(UcdStringTests, generalCategoryAndWhiteSpace) {
    std::string src = std::string(PRE) +
        "import cajeta.lang.Ucd;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        if (Ucd.generalCategory(65) != Ucd.GC_LU) { return -1; }\n"    // 'A'
        "        if (Ucd.generalCategory(97) != Ucd.GC_LL) { return -2; }\n"    // 'a'
        "        if (Ucd.generalCategory(49) != Ucd.GC_ND) { return -3; }\n"    // '1'
        "        if (Ucd.generalCategory(20013) != Ucd.GC_LO) { return -4; }\n" // 中
        "        if (Ucd.generalCategory(32) != Ucd.GC_ZS) { return -5; }\n"    // space
        "        if (Ucd.generalCategory(769) != Ucd.GC_MN) { return -6; }\n"   // U+0301
        "        if (Ucd.generalCategory(128512) != Ucd.GC_SO) { return -7; }\n" // 😀
        "        if (Ucd.generalCategory(44032) != Ucd.GC_LO) { return -8; }\n" // 가 (Hangul syl., UnicodeData range)
        "        if (Ucd.generalCategory(1114110) != Ucd.GC_CN) { return -9; }\n" // U+10FFFE noncharacter
        "        if (Ucd.isLetter(65) != 1) { return -10; }\n"
        "        if (Ucd.isLetter(20013) != 1) { return -11; }\n"
        "        if (Ucd.isLetter(49) != 0) { return -12; }\n"
        "        if (Ucd.isNumber(49) != 1) { return -13; }\n"
        "        if (Ucd.isNumber(8560) != 1) { return -14; }\n"  // ⅰ Nl
        "        if (Ucd.isNumber(97) != 0) { return -15; }\n"
        "        if (Ucd.isWhiteSpace(32) != 1) { return -16; }\n"
        "        if (Ucd.isWhiteSpace(9) != 1) { return -17; }\n"   // tab
        "        if (Ucd.isWhiteSpace(10) != 1) { return -18; }\n"  // LF
        "        if (Ucd.isWhiteSpace(160) != 1) { return -19; }\n" // NBSP
        "        if (Ucd.isWhiteSpace(8232) != 1) { return -20; }\n" // LSEP
        "        if (Ucd.isWhiteSpace(97) != 0) { return -21; }\n"
        "        if (Ucd.isWhiteSpace(8203) != 0) { return -22; }\n" // ZWSP is NOT ws
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
