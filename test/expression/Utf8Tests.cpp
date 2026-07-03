// Unit 6a of the slices plan: the Inline-only Utf8 value type (slice-spec §8)
// — 16-byte POD text, record/@ValueType-field-eligible, no rc.

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

// 6.1.1 (Inline form) — construct/read; value semantics (copies are independent
// snapshots of immutable text — share == copy trivially for a POD).
TEST(Utf8Tests, inlineConstructAndRead) {
    EXPECT_EQ(runJit(
        "Utf8 v = Utf8.of(\"EURUSD\");\n"
        "if (v.size() != 6) { return -1; }\n"
        "if (v.charAt(0) != (int8) 69) { return -2; }\n"    // 'E'
        "if (v.charAt(5) != (int8) 68) { return -3; }\n"    // 'D'
        "if (v.charAt(6) != (int8) 0) { return -4; }\n"     // OOB -> 0
        "Utf8 w = v;\n"                                       // value copy
        "if (!w.equals(v)) { return -5; }\n"
        "if (w.size() != 6) { return -6; }\n"
        "return 1;"), 1);
}

// 6.1.2 — clamp at the Inline cap (12 B) is the documented 6a behavior; a
// windowed (mode-2) source contributes its window bytes.
TEST(Utf8Tests, inlineCapAndWindowedSource) {
    EXPECT_EQ(runJit(
        "Utf8 cap = Utf8.of(\"abcdefghijklmnop\");\n"        // 16 -> clamps to 12
        "if (cap.size() != 12) { return -1; }\n"
        "if (cap.charAt(11) != (int8) 108) { return -2; }\n" // 'l'
        "String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
        "String b = \"0123456789\";\n"
        "String s = a + b;\n"
        "String w = s.substring(10, 16);\n"                   // "klmnop" (view)
        "Utf8 u = Utf8.of(w);\n"
        "if (u.size() != 6) { return -3; }\n"
        "if (!u.equalsString(\"klmnop\")) { return -4; }\n"
        "return 1;"), 1);
}

// 6.1.3 — equality/hash are representation-independent: same text from a
// literal, a concat, and a substring window all agree.
TEST(Utf8Tests, equalityHashRepresentationIndependent) {
    EXPECT_EQ(runJit(
        "Utf8 lit = Utf8.of(\"klmnop\");\n"
        "String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
        "String b = \"0123456789\";\n"
        "String s = a + b;\n"
        "Utf8 win = Utf8.of(s.substring(10, 16));\n"
        "if (!lit.equals(win)) { return -1; }\n"
        "if (lit.hash() != win.hash()) { return -2; }\n"
        "Utf8 other = Utf8.of(\"klmnoq\");\n"
        "if (lit.equals(other)) { return -3; }\n"
        "if (lit.hash() == other.hash()) { return -4; }\n"
        "return 1;"), 1);
}

// 6.1.5 (Inline scope) — Utf8 as a RECORD field (the records-spec §2.6.5
// convergence: a text-bearing schema). DISABLED: embedding a value type that
// itself contains an INLINE ARRAY field (Utf8.data int8[12]) SIGSEGVs (nil) in
// the record/value-type inline-embed path (merged from feature/records-unit2)
// — records with primitive/nested-record fields work (RecordTests 19/19), so
// the gap is specifically nested-value-type-with-inline-array. Repro preserved
// below; handed to the records track as a follow-up fix.
TEST(Utf8Tests, DISABLED_utf8AsRecordField) {
    std::string src =
        "package test;\n"
        "record Tick {\n"
        "    Utf8 venue;\n"
        "    float64 price;\n"
        "}\n"
        "public final class Ut {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        {\n"
        "            Utf8 nyse = Utf8.of(\"NYSE\");\n"
        "            Tick t = Tick { venue: nyse, price: 42.5 };\n"
        "            Tick u = t;\n"                          // value copy: memcpy
        "            if (u.price != 42.5) { return -1; }\n"
        "            if (!u.venue.equalsString(\"NYSE\")) { return -2; }\n"
        "            String round = u.venue.toString();\n"
        "            if (round.size() != 4) { return -3; }\n"
        "        }\n"
        "        int64 after = Cajeta.liveCount();\n"
        "        if (after != base) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}
