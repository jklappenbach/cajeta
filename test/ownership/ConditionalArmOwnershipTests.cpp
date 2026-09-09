// Unit 2 (spec §2.1) — the arms of a conditional and of a switch expression,
// classified by the one classifier: a local owns what the TAKEN arm produced.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const char* src, const char* cls) {
    auto jit = CajetaJit::compile(src, cls);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src.c_str(), "test.A");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code) << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public String name;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            this.name #= \"hello\" + 1;\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    static void take(#Cell c) { return; }\n";

} // namespace

// A switch with a borrow arm and a fresh arm: only the fresh cell is dropped.
TEST(ConditionalArmOwnershipTests, switchExpressionLocalOwnsOnlyTheTakenArm) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, int32 i) {\n"
        "        Cell k = switch (i) {\n"
        "            case 1 -> h.a;\n"
        "            case 2 -> h.b;\n"
        "            default -> heap Cell(9);\n"
        "        };\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.pick(h, 1) != 1) { return 1; }\n"
        "            if (A.pick(h, 2) != 2) { return 2; }\n"
        "            if (A.pick(h, 3) != 9) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 4; }\n"
        "        if (h.b.v != 2) { return 5; }\n"
        "        if (Cajeta.liveCount() != l0) { return 6; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1-3 = wrong arm; 4/5 = a borrowed field was freed; 6 = the fresh "
           "cells leaked or a field was freed twice";
}

// A String switch over a field read, a literal and a concat: only the concat
// is ever dropped.
TEST(ConditionalArmOwnershipTests, switchExpressionStringLocalOwnsOnlyTheConcat) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, int32 i) {\n"
        "        String s = switch (i) {\n"
        "            case 1 -> h.name;\n"
        "            case 2 -> \"-\";\n"
        "            default -> \"x\" + i;\n"
        "        };\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.pick(h, 1) != 6) { return 7; }\n"
        "            if (A.pick(h, 2) != 1) { return 8; }\n"
        "            if (A.pick(h, 3) != 2) { return 9; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!h.name.equals(\"hello1\")) { return 10; }\n"
        "        if (Cajeta.liveCount() != l0) { return 11; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "7-9 = wrong arm; 10 = the field's wrapper was freed; 11 = the "
           "concat leaked or a borrow was freed";
}

// A `#T` return walks the switch's arms: a field-read arm is rejected.
TEST(ConditionalArmOwnershipTests, ownedReturnOfSwitchWithBorrowArmIsRejected) {
    std::string src = std::string(PRE) +
        "    static #Cell pick(Holder h, int32 i) {\n"
        "        return switch (i) {\n"
        "            case 1 -> h.a;\n"
        "            default -> heap Cell(9);\n"
        "        };\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// A `#T` formal walks the switch's arms: a field-read arm is rejected.
TEST(ConditionalArmOwnershipTests, sharpFormalRejectsSwitchWithBorrowArm) {
    std::string src = std::string(PRE) +
        "    static void probe(Holder h, int32 i) {\n"
        "        A.take(switch (i) { case 1 -> h.a; default -> heap Cell(1); });\n"
        "        return;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// A frame-arena concat arm is not a title (spec 5.4): the local arms nothing.
TEST(ConditionalArmOwnershipTests, arenaConcatArmArmsNoDrop) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h, boolean c, int32 n) {\n"
        "        String s = c ? (\"a\" + n) : h.name;\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, true, i) < 2) { return 12; }\n"
        "            if (A.probe(h, false, i) != 6) { return 13; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!h.name.equals(\"hello1\")) { return 14; }\n"
        "        if (Cajeta.liveCount() != l0) { return 15; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "12/13 = wrong arm; 14 = the field's wrapper was freed; 15 = the "
           "concat arm leaked (heap concat with no entry) or was double-freed";
}
