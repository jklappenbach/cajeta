//
// silent-resolution diagnostics — Unit 4, "did you mean" suggestions (spec 4.2).
// Plan: agents/silent-resolution-diagnostics-plan.md
//
//   - 4.1.1 a near-miss member name suggests the real one.
//   - 4.1.2 a name with NO near match suggests nothing — no noise, no wrong guess.
//   - 4.1.3 inherited and synthesized members are ordinary members, so they are
//     candidates too.
//
// The suggestion rides on Unit 2's MEMBER_NOT_FOUND text; it never invents a new
// error, and it must never name a compiler-internal artifact (spec 4.3).
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPoint =
    "package test;\n"
    "public class Point {\n"
    "    public int32 volume;\n"
    "    public int32 scale(int32 k) { return k; }\n"
    "    public int32 volumize() { return 1; }\n"
    "}\n";

// Compile and hand back the diagnostic message (empty if it compiled).
std::string msgOf(const std::string& src) {
    try { CajetaJit::compile(src, "test.D"); }
    catch (cajeta::Exception& e) { return e.getMessage(); }
    return "";
}

std::string withPoint(const std::string& body) {
    return std::string(kPoint) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

} // namespace

// 4.1.1 — a one-edit method typo names the real method.
TEST(DidYouMeanTests, methodTypoSuggestsTheRealName) {
    std::string m = msgOf(withPoint(
        "Point p = heap Point();\n"
        "        return p.volumze();"));
    EXPECT_NE(m.find("did you mean"), std::string::npos)
        << "expected a suggestion, got: " << m;
    EXPECT_NE(m.find("volumize"), std::string::npos)
        << "expected 'volumize' suggested, got: " << m;
}

// 4.1.1 (field form) — the same for a misspelled FIELD.
TEST(DidYouMeanTests, fieldTypoSuggestsTheRealName) {
    std::string m = msgOf(withPoint(
        "Point p = heap Point();\n"
        "        return p.volme;"));
    EXPECT_NE(m.find("did you mean"), std::string::npos)
        << "expected a suggestion, got: " << m;
    EXPECT_NE(m.find("volume"), std::string::npos)
        << "expected 'volume' suggested, got: " << m;
}

// 4.1.2 — nothing close enough: stay silent rather than guess. A wrong
// suggestion is worse than none, because it sends the reader hunting.
TEST(DidYouMeanTests, noNearMatchSuggestsNothing) {
    std::string m = msgOf(withPoint(
        "Point p = heap Point();\n"
        "        return p.frobnicate();"));
    EXPECT_EQ(m.find("did you mean"), std::string::npos)
        << "expected NO suggestion, got: " << m;
}

// 4.1.2 (short-name guard) — a 2-edit distance over a SHORT name is most of the
// name, so it is not a near miss. `ab()` must not suggest `scale`/`volume`.
TEST(DidYouMeanTests, shortNameDoesNotDragInDistantMembers) {
    std::string m = msgOf(withPoint(
        "Point p = heap Point();\n"
        "        return p.ab();"));
    EXPECT_EQ(m.find("did you mean"), std::string::npos)
        << "expected NO suggestion for a short unrelated name, got: " << m;
}

// 4.1.3 — INHERITED members are ordinary members and must be candidates.
TEST(DidYouMeanTests, inheritedMemberIsSuggested) {
    std::string m = msgOf(
        "package test;\n"
        "public class Animal {\n"
        "    public int32 heartbeat() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 bark() { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        return d.heartbeet();\n"
        "    }\n"
        "}\n");
    EXPECT_NE(m.find("did you mean"), std::string::npos)
        << "expected a suggestion, got: " << m;
    EXPECT_NE(m.find("heartbeat"), std::string::npos)
        << "expected the INHERITED 'heartbeat' suggested, got: " << m;
}

// 4.3.1 — a suggestion must never name a compiler-internal artifact. The
// synthesized backing members (`__`-prefixed, vtable/rtti slots) are not things
// a developer can write, so they must not be offered.
TEST(DidYouMeanTests, neverSuggestsCompilerInternals) {
    std::string m = msgOf(withPoint(
        "Point p = heap Point();\n"
        "        return p.__vtable();"));
    EXPECT_EQ(m.find("did you mean '__"), std::string::npos)
        << "must not suggest a compiler-internal member, got: " << m;
}
