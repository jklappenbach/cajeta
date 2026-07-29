//
// Ternary (`?:`) result as a String concatenation operand (ternary-string-concat
// plan Unit 1; spec §2). `"x" + (cond ? "a" : "b")` rendered the ternary operand
// EMPTY because loadIfLValue loaded through the ternary's r-value String pointer
// (the vtable word) instead of using it. These pin the fix.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstring>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Run a String-returning `run()` and compare its window against `expected`.
// 6.2.2 tagged core: { vtable@0, i32 lenTag@8, i32 aux@12, ptr base@16,
// i32 cachedCpLength@24 } — Inline text at +12 for len <= 12, else
// base + 8 + aux.
void expectString(CajetaJit* jit, const char* sym, const char* expected) {
    auto fn = jit->lookup<void* (*)()>(sym);
    ASSERT_NE(fn, nullptr) << sym;
    char* s = reinterpret_cast<char*>(fn());
    ASSERT_NE(s, nullptr) << sym << " returned null";
    int32_t lenTag = *reinterpret_cast<int32_t*>(s + 8);
    int32_t byteLen = lenTag & 0x1FFFFFFF;
    const char* data;
    if (byteLen <= 12) {
        data = s + 12;
    } else {
        char* base = *reinterpret_cast<char**>(s + 16);
        ASSERT_NE(base, nullptr) << sym << " has null root";
        data = base + 8 + *reinterpret_cast<int32_t*>(s + 12);
    }
    EXPECT_EQ(std::string(data, data + byteLen), std::string(expected)) << sym;
}

} // namespace

// 2.1.1 — ternary on the RIGHT of the concat, literal arms.
TEST(TernaryStringConcatTests, TernaryRightLiteralArms) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static #String pick(boolean c) { return \"x\" + (c ? \"a\" : \"b\"); }\n"
        "    public static #String runTrue()  { return pick(true); }\n"
        "    public static #String runFalse() { return pick(false); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    expectString(jit.get(), "runTrue", "xa");
    expectString(jit.get(), "runFalse", "xb");
}

// 2.1.2 — ternary on the LEFT.
TEST(TernaryStringConcatTests, TernaryLeft) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static #String pick(boolean c) { return (c ? \"a\" : \"b\") + \"x\"; }\n"
        "    public static #String runTrue()  { return pick(true); }\n"
        "    public static #String runFalse() { return pick(false); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    expectString(jit.get(), "runTrue", "ax");
    expectString(jit.get(), "runFalse", "bx");
}

// 2.1.3 — non-literal String arms (each arm is itself a concat).
TEST(TernaryStringConcatTests, NonLiteralArms) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static #String pick(boolean c, String p) {\n"
        "        return \"<\" + (c ? (p + \"-hi\") : (p + \"-lo\")) + \">\";\n"
        "    }\n"
        "    public static #String runTrue()  { return pick(true,  \"k\"); }\n"
        "    public static #String runFalse() { return pick(false, \"k\"); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    expectString(jit.get(), "runTrue", "<k-hi>");
    expectString(jit.get(), "runFalse", "<k-lo>");
}

// 2.1.4 — assign a String ternary to a var, then concat it (carries real value).
TEST(TernaryStringConcatTests, AssignedThenConcatenated) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static #String pick(boolean c) {\n"
        "        String mid = c ? \"YES\" : \"NO\";\n"
        "        return \"[\" + mid + \"]\";\n"
        "    }\n"
        "    public static #String runTrue()  { return pick(true); }\n"
        "    public static #String runFalse() { return pick(false); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    expectString(jit.get(), "runTrue", "[YES]");
    expectString(jit.get(), "runFalse", "[NO]");
}

// 2.1.5 — non-regression: an int ternary in a concat still stringifies correctly.
TEST(TernaryStringConcatTests, IntTernaryInConcatUnchanged) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static #String pick(boolean c) { return \"n=\" + (c ? 1 : 2); }\n"
        "    public static #String runTrue()  { return pick(true); }\n"
        "    public static #String runFalse() { return pick(false); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    expectString(jit.get(), "runTrue", "n=1");
    expectString(jit.get(), "runFalse", "n=2");
}
