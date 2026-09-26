// A String local bound to one literal, reassigned another, and returned
// owned (`return #s`) must reach the caller intact. Found 2026-09-26 by
// primavera's pipeline self-test: the returned String read as a dead
// pointer (SIGSEGV at 0x2b) while the same function written with one literal
// per branch was fine.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {
std::string runText(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return "<null>";
    int64_t count = *(int64_t*) hdr;
    const char* data = (const char*) hdr + 8;
    return std::string(data, data + count);
}
}  // namespace

TEST(OwnedReturnOfReassignedLiteralTests, reassignedLiteralReturnsIntact) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String name(int32 kind) {\n"
        "        String s = \"\";\n"
        "        if (kind == 0) { s = \"raw\"; }\n"
        "        else if (kind == 1) { s = \"decoded\"; }\n"
        "        else { s = \"kind-\" + (int64) kind; }\n"
        "        return #s;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String a #= D.name(0);\n"
        "        String b #= D.name(1);\n"
        "        String c #= D.name(7);\n"
        "        String all = a + \"|\" + b + \"|\" + c;\n"
        "        return all.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "raw|decoded|kind-7");
}

TEST(OwnedReturnOfReassignedLiteralTests, oneLiteralPerBranchIsTheControl) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String name(int32 kind) {\n"
        "        if (kind == 0) { String raw = \"raw\"; return #raw; }\n"
        "        if (kind == 1) { String dec = \"decoded\"; return #dec; }\n"
        "        String other = \"kind-\" + (int64) kind;\n"
        "        return #other;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String a #= D.name(0);\n"
        "        String b #= D.name(1);\n"
        "        String c #= D.name(7);\n"
        "        String all = a + \"|\" + b + \"|\" + c;\n"
        "        return all.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "raw|decoded|kind-7");
}

TEST(OwnedReturnOfReassignedLiteralTests, reassignedLiteralUsedInConcatenation) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String name(int32 kind) {\n"
        "        String s = \"\";\n"
        "        if (kind == 0) { s = \"raw\"; }\n"
        "        return #s;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String m = \"pipeline \" + D.name(0) + \" x\";\n"
        "        return m.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "pipeline raw x");
}

// Narrowing: the same function with a literal branch and an owned branch,
// called through one path at a time.
TEST(OwnedReturnOfReassignedLiteralTests, mixedFunctionLiteralPathOnly) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String name(int32 kind) {\n"
        "        String s = \"\";\n"
        "        if (kind == 0) { s = \"raw\"; }\n"
        "        else { s = \"kind-\" + (int64) kind; }\n"
        "        return #s;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String a #= D.name(0);\n"
        "        return a.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "raw");
}

TEST(OwnedReturnOfReassignedLiteralTests, mixedFunctionOwnedPathOnly) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String name(int32 kind) {\n"
        "        String s = \"\";\n"
        "        if (kind == 0) { s = \"raw\"; }\n"
        "        else { s = \"kind-\" + (int64) kind; }\n"
        "        return #s;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String c #= D.name(7);\n"
        "        return c.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "kind-7");
}

TEST(OwnedReturnOfReassignedLiteralTests, ownedThenLiteralReassignNoReturn) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        String s = \"kind-\" + (int64) 7;\n"
        "        s = \"raw\";\n"
        "        String t = \"\";\n"
        "        t = \"kind-\" + (int64) 7;\n"
        "        String all = s + \"|\" + t;\n"
        "        return all.toBytes();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runText(src), "raw|kind-7");
}
