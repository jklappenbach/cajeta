//
// Switch + throw/try/catch + for-init-declaration tests.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class S {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- for-init variable declaration -------------------------------------------

TEST(ForInitDeclTests, declareInsideInit) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 sum = 0;\n"
        "for (int32 i = 0; i < 5; i = i + 1) {\n"
        "    sum = sum + i;\n"
        "}\n"
        "return sum;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 0+1+2+3+4 = 10
    EXPECT_EQ(fn(), 10);
}

TEST(ForInitDeclTests, declareWithBreak) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 found = -1;\n"
        "for (int32 i = 0; i < 100; i = i + 1) {\n"
        "    if (i * i > 50) {\n"
        "        found = i;\n"
        "        break;\n"
        "    }\n"
        "}\n"
        "return found;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // First i where i*i > 50 is 8 (8*8=64).
    EXPECT_EQ(fn(), 8);
}

// --- switch ------------------------------------------------------------------

TEST(SwitchTests, basicCaseMatch) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 2;\n"
        "int32 result = 0;\n"
        "switch (x) {\n"
        "    case 1: result = 10; break;\n"
        "    case 2: result = 20; break;\n"
        "    case 3: result = 30; break;\n"
        "    default: result = 99; break;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 20);
}

TEST(SwitchTests, defaultBranch) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 42;\n"
        "int32 result = 0;\n"
        "switch (x) {\n"
        "    case 1: result = 10; break;\n"
        "    default: result = 99; break;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

TEST(SwitchTests, fallthroughWithoutBreak) {
    // `case 1:` falls through to `case 2:`'s body, which then breaks.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 1;\n"
        "int32 result = 0;\n"
        "switch (x) {\n"
        "    case 1:\n"
        "    case 2: result = 99; break;\n"
        "    case 3: result = 100; break;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

TEST(SwitchTests, fallsThroughBodies) {
    // No break between case 1 and case 2 — both bodies run.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 1;\n"
        "int32 result = 0;\n"
        "switch (x) {\n"
        "    case 1: result = result + 1;\n"
        "    case 2: result = result + 10; break;\n"
        "    case 3: result = result + 100; break;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // x=1 enters case 1, adds 1, falls through to case 2, adds 10. Result: 11.
    EXPECT_EQ(fn(), 11);
}

TEST(SwitchTests, noMatchingCaseNoDefault) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 99;\n"
        "int32 result = -1;\n"
        "switch (x) {\n"
        "    case 1: result = 10; break;\n"
        "    case 2: result = 20; break;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), -1);
}

// --- throw + try/catch -------------------------------------------------------

TEST(TryCatchTests, throwCaughtInSameMethod) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 result = 0;\n"
        "try {\n"
        "    throw 42;\n"
        "} catch (Exception e) {\n"
        "    result = 7;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

TEST(TryCatchTests, throwValueAvailableInCatch) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int64 result = 0;\n"
        "try {\n"
        "    throw 12345;\n"
        "} catch (Exception e) {\n"
        "    result = e;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 12345);
}

TEST(TryCatchTests, noThrowSkipsCatch) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 result = 0;\n"
        "try {\n"
        "    result = 100;\n"
        "} catch (Exception e) {\n"
        "    result = -1;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

TEST(TryCatchTests, throwInsideConditional) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 5;\n"
        "int32 result = 0;\n"
        "try {\n"
        "    if (x > 0) {\n"
        "        throw 99;\n"
        "    }\n"
        "    result = 1;\n"
        "} catch (Exception e) {\n"
        "    result = 2;\n"
        "}\n"
        "return result;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

TEST(TryCatchTests, throwInsideLoop) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 count = 0;\n"
        "try {\n"
        "    for (int32 i = 0; i < 100; i = i + 1) {\n"
        "        if (i == 5) {\n"
        "            throw i;\n"
        "        }\n"
        "        count = count + 1;\n"
        "    }\n"
        "} catch (Exception e) {\n"
        "}\n"
        "return count;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // Loop runs i=0..4 (count=5), then throws at i=5.
    EXPECT_EQ(fn(), 5);
}
