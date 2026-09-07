//
// Ownership of a local bound from a ternary (`T x = c ? a : b`).
//
// Measured 2026-09-07 (cajeta-llm prefill-routes 2.1.1, tmp/probe-emit):
// `String nm = r.name != null ? r.name : "-";` gave the local an ARMED
// drop entry that freed the field's wrapper at scope end, while
// `String nm = r.name;` got none. LocalVariableDeclaration classified an
// initializer as a borrow only for the shapes it recognised (literal,
// identifier, field read, element read, plain-return call); a ternary
// matched none and defaulted to owned. Every diagnostic record under
// cajeta-llm's trace/debug logging double-freed a String through that line,
// and the freed block's reuse showed up as GPU page faults at host
// addresses — a "device race" that was this host miscompile.
//
// The rule these pin: the local owns exactly what the TAKEN arm produced.
// Two borrow arms -> no drop. A heap/concat/`#x` arm -> dropped when taken,
// never when the borrow arm was taken. The verdicts are read back through
// the runtime: a wrongly freed wrapper is poisoned, so the field's bytes no
// longer compare equal; a leaked temporary moves Cajeta.liveCount().
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const char* src, const char* cls) {
    auto jit = CajetaJit::compile(src, cls);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Rec {\n"
    "        public String name;\n"
    "        public Rec() { return; }\n"
    "    }\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            return;\n"
    "        }\n"
    "    }\n";

} // namespace

// A String local from a ternary whose BOTH arms are borrows (a field read
// and a literal) must not free the field. This is the measured bug.
TEST(TernaryOwnershipTests, stringLocalFromBorrowArmsDoesNotFreeTheField) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        String nm = c ? r.name : \"-\";\n"
        "        return nm.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            A.probe(r, true);\n"
        "            A.probe(r, false);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!r.name.equals(\"hello1\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1 = the field's bytes no longer read back: the local dropped a borrow";
}

// Mixed arms: the concat is dropped exactly when it was taken (no leak),
// and the field is never freed when the borrow arm was taken.
TEST(TernaryOwnershipTests, stringLocalFromMixedArmsDropsOnlyWhatItOwns) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        String s = c ? (\"x\" + 7) : r.name;\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.probe(r, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return 2; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.probe(r, false); i = i + 1; }\n"
        "        if (!r.name.equals(\"hello1\")) { return 3; }\n"
        "        if (Cajeta.liveCount() != l0) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "2 = the taken concat leaked; 3 = the borrow arm freed the field; "
           "4 = live count moved on the borrow arm";
}

// A class-typed local from two field-read arms must not free either object.
TEST(TernaryOwnershipTests, classLocalFromBorrowArmsDoesNotFreeEither) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, boolean c) {\n"
        "        Cell k = c ? h.a : h.b;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            A.pick(h, true);\n"
        "            A.pick(h, false);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 5; }\n"
        "        if (h.b.v != 2) { return 6; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "5/6 = a field object no longer reads back: the local dropped a borrow";
}

// Mixed class arms: the heap object is dropped when taken, the field never.
TEST(TernaryOwnershipTests, classLocalFromMixedArmsDropsOnlyWhatItOwns) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, boolean c) {\n"
        "        Cell k = c ? heap Cell(9) : h.a;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.pick(h, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return 7; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.pick(h, false); i = i + 1; }\n"
        "        if (h.a.v != 1) { return 8; }\n"
        "        if (Cajeta.liveCount() != l0) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "7 = the taken heap object leaked; 8 = the borrow arm freed the field; "
           "9 = live count moved on the borrow arm";
}
