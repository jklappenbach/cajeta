// Unit 5 of the slices plan: Object.clone() goes live (slice-spec §6.4).
// Shallow copy via the RTTI field walk; String fields become fresh stakes on
// the shared buffer; a String receiver detaches (materialized owned copy).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Rec {\n"
           "    public int64 a;\n"
           "    public String s;\n"
           "    public Rec(int64 a, #String s) { this.a = a; this.s #= s; }\n"
           "}\n"
           "public final class Oct {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Oct");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kDyn =
    "String x = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "String y = \"0123456789\";\n"
    "String s = x + y;\n";

}  // namespace

// 5.1.1 — clone of an object with value + String fields: independent value
// fields, String field shares the bytes via a fresh stake; both drop cleanly
// (exact liveCount balance — no leak, no double free).
TEST(ObjectCloneTests, cloneSharesStringBufferSafely) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "{\n" +
        std::string(kDyn) +
        "    String w #= s.substring(5, 25);\n"        // windowed view field
        "    Rec r1 = heap Rec(7, #w);\n"
        "    Rec r2 = (Rec) r1.clone();\n"
        "    if (r2 == null) { return -1; }\n"
        "    r1.a = 99;\n"
        "    if (r2.a != 7) { return -2; }\n"          // value field independent
        "    if (r2.s.size() != 20) { return -3; }\n"
        "    if (!r2.s.equals(r1.s)) { return -4; }\n" // same window bytes
        "}\n"
        "int64 after = Cajeta.liveCount();\n"
        "if (after != base) { return -5; }\n"
        "return 1;"), 1);
}

// 5.1.3 — clone() on a windowed String DETACHES: content preserved after the
// source and the view drop; balance exact.
TEST(ObjectCloneTests, sliceCloneDetaches) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "{\n" +
        std::string(kDyn) +
        "    String w #= s.substring(10, 20);\n"        // "klmnopqrst"
        "    String d = (String) w.clone();\n"         // detach
        "    s = \"\";\n"                              // root's owner gone
        "    if (!d.equals(\"klmnopqrst\")) { return -1; }\n"
        "    if (d.size() != 10) { return -2; }\n"
        "}\n"
        "int64 after = Cajeta.liveCount();\n"
        "if (after != base) { return -3; }\n"
        "return 1;"), 1);
}

// 5.1.2 — a user override replaces the native default and is dispatched.
TEST(ObjectCloneTests, cloneOverrideDispatches) {
    std::string src =
        "package test;\n"
        "public final class Ov {\n"
        "    public int64 marker;\n"
        "    public #Object clone() {\n"
        "        Ov o = heap Ov();\n"
        "        o.marker = 42;\n"
        "        return o;\n"
        "    }\n"
        "}\n"
        "public final class Oct {\n"
        "    public static int32 run() {\n"
        "        Ov v = heap Ov();\n"
        "        v.marker = 7;\n"
        "        Ov c = (Ov) v.clone();\n"
        "        if (c.marker != 42) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Oct");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}
