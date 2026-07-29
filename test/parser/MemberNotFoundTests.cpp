//
// silent-resolution diagnostics — Unit 2, member-not-found is a real diagnostic.
// Plan: agents/silent-resolution-diagnostics-plan.md
//
// Unit 1 made an unresolvable member FAIL the compile, but only via the
// downstream backstop ("returned expression did not resolve to a value"),
// which names neither the member nor the receiver. Unit 2 diagnoses it at the
// RESOLUTION site, in the shape the Vector path already emits:
//
//   2.1.1 misspelled method  -> names the method + the receiver's canonical type
//   2.1.2 misspelled field   -> located at the field's span
//   2.1.3 overload mismatch  -> DISTINCT error listing candidates, not "no such member"
//   2.1.4 synthesized member -> identical treatment to a hand-written one
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPoint =
    "package test;\n"
    "public class Point {\n"
    "    public int32 v = 7;\n"
    "    public int32 volume() { return v; }\n"
    "    public int32 scale(int32 k) { return v * k; }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

// Compile `src`, expect a cajeta::Exception, hand it back for inspection.
cajeta::Exception expectThrows(const std::string& src) {
    try {
        runI32(src);
    } catch (cajeta::Exception& e) {
        return e;
    }
    ADD_FAILURE() << "expected the compile to fail";
    return cajeta::Exception("no error was thrown", "NONE");
}

} // namespace

// 2.1.1: the method name and the receiver's canonical type both appear.
TEST(MemberNotFoundTests, misspelledMethodNamesMemberAndReceiver) {
    auto e = expectThrows(std::string(kPoint) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point();\n"
        "        return p.volme();\n"
        "    }\n"
        "}\n");

    EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MEMBER_NOT_FOUND");
    EXPECT_NE(e.getMessage().find("volme"), std::string::npos)
        << "must name the missing method: " << e.getMessage();
    EXPECT_NE(e.getMessage().find("test.Point"), std::string::npos)
        << "must name the receiver's canonical type: " << e.getMessage();
    EXPECT_TRUE(e.hasLocation());
}

// 2.1.2: a misspelled FIELD is diagnosed too, at its own span.
TEST(MemberNotFoundTests, misspelledFieldIsDiagnosed) {
    auto e = expectThrows(std::string(kPoint) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point();\n"
        "        return p.vee;\n"
        "    }\n"
        "}\n");

    EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MEMBER_NOT_FOUND");
    EXPECT_NE(e.getMessage().find("vee"), std::string::npos)
        << "must name the missing field: " << e.getMessage();
    EXPECT_TRUE(e.hasLocation());
}

// 2.1.3: the name EXISTS but no overload accepts these arguments. That is a
// different mistake from "no such member" and must not be reported as one --
// telling a user `scale` does not exist, when it does, sends them the wrong way.
TEST(MemberNotFoundTests, overloadMismatchIsDistinctFromMemberNotFound) {
    auto e = expectThrows(std::string(kPoint) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point();\n"
        "        return p.scale();\n"   // scale(int32) exists; scale() does not
        "    }\n"
        "}\n");

    EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_OVERLOAD");
    EXPECT_NE(e.getMessage().find("scale"), std::string::npos) << e.getMessage();
    // The candidate the user probably meant is shown.
    EXPECT_NE(e.getMessage().find("int32"), std::string::npos)
        << "must list the candidate signature(s): " << e.getMessage();
    EXPECT_TRUE(e.hasLocation());
}

// 2.1.4 lives in ColumnSynthesisTests.callSiteTypoIsACompileError -- it needs
// the Columns<T> preamble, and the point of the test is that the SYNTHESIZED
// member yields the same CAJETA_ERROR_MEMBER_NOT_FOUND as the hand-written miss
// pinned above. Asserting the same id in both places is the whole claim.
