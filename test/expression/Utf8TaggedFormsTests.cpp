// Unit 6b of the slices plan: the Static/Shared Utf8 tagged forms + the
// synthesized value-copy/drop hooks (slice-spec §8, §6.1; plan 6.1.1/6.1.3
// full scope + 3.1.4/3.2.x moved here from Unit 3).
//
// TDD — written before the 6b coding lands. Expected to fail (compile or
// behavior) until: (a) Utf8.of lifts the 12-byte clamp into the pointer
// forms, (b) copies of a Shared Utf8 retain / drops release via synthesized
// hooks, (c) hash switches to XXH String-parity, (d) the
// Cajeta.sharedPopulation() intrinsic exists (wraps
// __cajeta_shared_population).
//
// Deliberately NOT asserted here: liveCount balance through Utf8.toString()
// — that path sits on the `heap String(#buf, n)` view-ctor orphan (plan 9.1);
// and explicit `#`-move of a value-type local (move semantics for value
// aggregates are the auto-move story, Unit 4 — 3.2.3's no-retain move path is
// covered by regression + the String-side 3.1.2 proof).

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

// 6.1.1 (Static form) — a >12 B literal keeps its full length with no clamp,
// no heap allocation, and no rc entry: the value windows the literal bytes.
TEST(Utf8TaggedFormsTests, staticFormLongLiteral) {
    EXPECT_EQ(runJit(
        "int64 pop = Cajeta.sharedPopulation();\n"
        "int64 before = Cajeta.allocatedBytes();\n"
        "Utf8 v = Utf8.of(\"abcdefghijklmnopqrstuvwxyz\");\n"   // 26 B literal
        "if (v.size() != 26) { return -1; }\n"
        "if (v.charAt(0) != (int8) 97) { return -2; }\n"        // 'a'
        "if (v.charAt(25) != (int8) 122) { return -3; }\n"      // 'z'
        "if (v.charAt(26) != (int8) 0) { return -4; }\n"        // OOB -> 0
        "Utf8 w = v;\n"                                           // free copy
        "if (!w.equals(v)) { return -5; }\n"
        "if (Cajeta.allocatedBytes() != before) { return -6; }\n"
        "if (Cajeta.sharedPopulation() != pop) { return -7; }\n"
        "return 1;"), 1);
}

// 6.1.1 (Shared form) — a >12 B dynamic source promotes: the Utf8 holds a
// stake and OUTLIVES the source String (the core zero-copy promise), with no
// byte copy of the buffer.
TEST(Utf8TaggedFormsTests, sharedFormOutlivesSource) {
    EXPECT_EQ(runJit(
        "int64 pop = Cajeta.sharedPopulation();\n"
        "Utf8 v = stack Utf8();\n"
        "{\n"
        "    String a = \"abcdefghijklm\";\n"
        "    String b = \"nopqrstuvwxyz\";\n"
        "    String s = a + b;\n"                                 // 26 B dynamic
        "    int64 before = Cajeta.allocatedBytes();\n"
        "    v = Utf8.of(s);\n"
        "    if (Cajeta.allocatedBytes() - before > 64) { return -1; }\n" // no 26B copy path
        "    if (Cajeta.sharedPopulation() != pop + 1) { return -2; }\n"
        "}\n"                                                     // source drops here
        "if (v.size() != 26) { return -3; }\n"
        "if (v.charAt(13) != (int8) 110) { return -4; }\n"        // 'n'
        "if (!v.equalsString(\"abcdefghijklmnopqrstuvwxyz\")) { return -5; }\n"
        "return 1;"), 1);
}

// 3.2.2/6.1.5 (drop hook) — the last Utf8 stake dropping releases the buffer:
// exact liveCount + shared-population balance across the scope.
TEST(Utf8TaggedFormsTests, sharedDropReleases) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "{\n"
        "    String a = \"abcdefghijklm\";\n"
        "    String b = \"nopqrstuvwxyz\";\n"
        "    String s = a + b;\n"
        "    Utf8 v = Utf8.of(s);\n"
        "    if (v.size() != 26) { return -1; }\n"
        "}\n"
        "if (Cajeta.liveCount() != base) { return -2; }\n"
        "if (Cajeta.sharedPopulation() != pop) { return -3; }\n"
        "return 1;"), 1);
}

// 3.2.2 (copy hook) — copying a Shared Utf8 retains (both independent stakes;
// dropping one leaves the other readable; dropping both balances exactly).
TEST(Utf8TaggedFormsTests, copyRetainsSharedForm) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "Utf8 keep = stack Utf8();\n"
        "{\n"
        "    String a = \"abcdefghijklm\";\n"
        "    String b = \"nopqrstuvwxyz\";\n"
        "    String s = a + b;\n"
        "    Utf8 v = Utf8.of(s);\n"
        "    keep = v;\n"                                         // copy: retain
        "}\n"                                                     // v + s drop
        "if (!keep.equalsString(\"abcdefghijklmnopqrstuvwxyz\")) { return -1; }\n"
        "if (Cajeta.sharedPopulation() != pop + 1) { return -2; }\n"
        "keep = Utf8.of(\"x\");\n"                                // overwrite: release
        "if (Cajeta.liveCount() != base) { return -3; }\n"
        "if (Cajeta.sharedPopulation() != pop) { return -4; }\n"
        "return 1;"), 1);
}

// 6.1.2 (full normalization rule) — a ≤12 B result is Inline REGARDLESS of
// source form: building from a long dynamic string's window takes no stake.
TEST(Utf8TaggedFormsTests, normalizationInlineFromDynamicWindow) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "Utf8 v = stack Utf8();\n"
        "{\n"
        "    String a = \"abcdefghijklm\";\n"
        "    String b = \"nopqrstuvwxyz\";\n"
        "    String s = a + b;\n"
        "    String w #= s.substring(10, 16);\n"                   // "klmnop": 6 B
        "    v = Utf8.of(w);\n"
        "    if (Cajeta.sharedPopulation() < pop) { return -1; }\n"
        "}\n"
        "if (v.size() != 6) { return -2; }\n"
        "if (!v.equalsString(\"klmnop\")) { return -3; }\n"
        "if (Cajeta.sharedPopulation() != pop) { return -4; }\n"  // Inline holds no stake
        "if (Cajeta.liveCount() != base) { return -5; }\n"
        "return 1;"), 1);
}

// 6.1.3 (full scope) — equality is representation-independent ACROSS forms:
// Inline vs Static vs Shared with equal text agree; unequal text disagrees.
TEST(Utf8TaggedFormsTests, equalityAcrossForms) {
    EXPECT_EQ(runJit(
        "Utf8 stat = Utf8.of(\"abcdefghijklmnopqrstuvwxyz\");\n"  // Static
        "String a = \"abcdefghijklm\";\n"
        "String b = \"nopqrstuvwxyz\";\n"
        "String s = a + b;\n"
        "Utf8 shar = Utf8.of(s);\n"                               // Shared
        "if (!stat.equals(shar)) { return -1; }\n"
        "if (!shar.equals(stat)) { return -2; }\n"
        "Utf8 shortLit = Utf8.of(\"klmnop\");\n"                  // Inline
        "Utf8 shortWin = Utf8.of(s.substring(10, 16));\n"         // Inline (normalized)
        "if (!shortLit.equals(shortWin)) { return -3; }\n"
        "if (stat.equals(shortLit)) { return -4; }\n"             // length differs
        "String t = s + \"!\";\n"
        "Utf8 bang = Utf8.of(t);\n"
        "if (stat.equals(bang)) { return -5; }\n"
        "return 1;"), 1);
}

// 6.1.3 (XXH String-parity) — Utf8.hash() equals String.hash() for the same
// text, whatever the form; unequal text hashes unequal.
TEST(Utf8TaggedFormsTests, hashXxhStringParity) {
    EXPECT_EQ(runJit(
        "String lit = \"abcdefghijklmnopqrstuvwxyz\";\n"
        "Utf8 stat = Utf8.of(lit);\n"
        "if (stat.hash() != lit.hash()) { return -1; }\n"
        "String a = \"abcdefghijklm\";\n"
        "String b = \"nopqrstuvwxyz\";\n"
        "String s = a + b;\n"
        "Utf8 shar = Utf8.of(s);\n"
        "if (shar.hash() != lit.hash()) { return -2; }\n"
        "String shortStr = \"klmnop\";\n"
        "Utf8 inl = Utf8.of(shortStr);\n"
        "if (inl.hash() != shortStr.hash()) { return -3; }\n"
        "if (inl.hash() == lit.hash()) { return -4; }\n"
        "return 1;"), 1);
}

// 3.1.4 — bounded copy never traverses: copying an aggregate holding Shared
// Utf8 fields is O(fields) — retains fire, no buffer byte copies.
TEST(Utf8TaggedFormsTests, boundedCopyNeverTraverses) {
    std::string src =
        "package test;\n"
        "record Quote {\n"
        "    Utf8 sym;\n"
        "    Utf8 venue;\n"
        "    float64 px;\n"
        "}\n"
        "public final class Ut {\n"
        "    public static int32 run() {\n"
        "        int64 pop = Cajeta.sharedPopulation();\n"
        "        String a = \"abcdefghijklm\";\n"
        "        String b = \"nopqrstuvwxyz\";\n"
        "        String s = a + b;\n"
        "        String v = s + s;\n"                             // 52 B, distinct base
        "        Quote q = Quote { sym: Utf8.of(s), venue: Utf8.of(v), px: 1.5 };\n"
        "        if (Cajeta.sharedPopulation() != pop + 2) { return -1; }\n"
        "        int64 before = Cajeta.allocatedBytes();\n"
        "        Quote r = q;\n"                                  // copy: 2 retains, 0 byte copies
        "        if (Cajeta.allocatedBytes() != before) { return -2; }\n"
        "        if (!r.sym.equalsString(\"abcdefghijklmnopqrstuvwxyz\")) { return -3; }\n"
        "        if (r.px != 1.5) { return -4; }\n"
        "        if (Cajeta.sharedPopulation() != pop + 2) { return -5; }\n" // same 2 bases
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}

// 3.2.2 (field-store path) — a heap class holding a Shared Utf8 field: the
// ctor's field store retains (copy hook), the local's scope-exit releases,
// and the object's drop chain fires the field's drop hook — balance exact.
// (The arg is a NAMED local: an rvalue call-result arg leaves a temp-owned
// stake nobody releases — the temp-stake cleanup is a recorded follow-up,
// same family as the String temp-wrapper drop gap.)
TEST(Utf8TaggedFormsTests, sharedFieldReleasesOnObjectDrop) {
    std::string src =
        "package test;\n"
        "public class Holder {\n"
        "    public Utf8 tag;\n"
        "    public Holder(Utf8 tag) {\n"
        "        this.tag = tag;\n"
        "    }\n"
        "}\n"
        "public final class Ut {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 pop = Cajeta.sharedPopulation();\n"
        "        {\n"
        "            String a = \"abcdefghijklm\";\n"
        "            String b = \"nopqrstuvwxyz\";\n"
        "            String s = a + b;\n"
        "            Utf8 t = Utf8.of(s);\n"
        "            Holder h = heap Holder(t);\n"
        "            if (!h.tag.equalsString(\"abcdefghijklmnopqrstuvwxyz\")) { return -1; }\n"
        "            if (Cajeta.sharedPopulation() < pop + 1) { return -2; }\n"
        "        }\n"
        "        if (Cajeta.liveCount() != base) { return -3; }\n"
        "        if (Cajeta.sharedPopulation() != pop) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}
