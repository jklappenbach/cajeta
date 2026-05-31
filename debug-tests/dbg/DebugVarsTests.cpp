//
// CP5 tests for the host-side value layer (DebugVars) and the runtime debug
// frame chain it reads. Two layers:
//  1) pure formatValue/writeValue — microsecond, no JIT.
//  2) a runtime chain test: drive __cajeta_dbg_frame_enter/local/leave against
//     the native runtime copy, then walk it with walkFrames and assert
//     depth/names/values. No JIT compile.
//
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cajeta/dbg/DebugVars.h"

using cajeta::dbg::walkFrames;
using cajeta::dbg::formatValue;
using cajeta::dbg::writeValue;
using cajeta::dbg::evaluateCondition;
using cajeta::dbg::DbgFrameInfo;
using cajeta::dbg::DbgVar;

// Native runtime frame-chain builders + the head selector (native copy).
extern "C" {
    void __cajeta_dbg_frame_enter(const char* func);
    void __cajeta_dbg_frame_leave(void);
    void __cajeta_dbg_local(const char* name, const char* type, void* addr);
    void** __cajeta_dbg_top_ptr(void);
}

// ---- pure formatValue ----

TEST(DebugVarsFormat, IntegerWidths) {
    int32_t i32 = 5;          EXPECT_EQ(formatValue("int32", &i32), "5");
    int32_t neg = -7;         EXPECT_EQ(formatValue("int32", &neg), "-7");
    int64_t i64 = 9000000000; EXPECT_EQ(formatValue("int64", &i64), "9000000000");
    uint32_t u32 = 4000000000u; EXPECT_EQ(formatValue("uint32", &u32), "4000000000");
    int16_t i16 = -300;       EXPECT_EQ(formatValue("int16", &i16), "-300");
    uint8_t u8 = 200;         EXPECT_EQ(formatValue("uint8", &u8), "200");
}

TEST(DebugVarsFormat, BooleanAndChar) {
    uint8_t t = 1, f = 0;
    EXPECT_EQ(formatValue("boolean", &t), "true");
    EXPECT_EQ(formatValue("boolean", &f), "false");
    int32_t c = 65;   // 'A' codepoint
    EXPECT_EQ(formatValue("char", &c), "65");
}

TEST(DebugVarsFormat, Floats) {
    float f = 1.5f;   EXPECT_EQ(formatValue("float32", &f), "1.5");
    double d = 2.25;  EXPECT_EQ(formatValue("float64", &d), "2.25");
}

TEST(DebugVarsFormat, ObjectIsOpaque) {
    int dummy = 0;
    void* slot = &dummy;  // slot "holds" a pointer
    std::string s = formatValue("demo.Foo", &slot);
    EXPECT_EQ(s.rfind("<demo.Foo@", 0), 0u);
    EXPECT_EQ(s.back(), '>');
}

// ---- pure writeValue ----

TEST(DebugVarsWrite, PrimitiveRoundTrip) {
    int32_t i = 0;
    std::string err;
    ASSERT_TRUE(writeValue("int32", &i, "42", &err)) << err;
    EXPECT_EQ(i, 42);
    EXPECT_EQ(formatValue("int32", &i), "42");

    double d = 0;
    ASSERT_TRUE(writeValue("float64", &d, "3.5", &err)) << err;
    EXPECT_DOUBLE_EQ(d, 3.5);

    uint8_t b = 0;
    ASSERT_TRUE(writeValue("boolean", &b, "true", &err)) << err;
    EXPECT_EQ(b, 1);
}

TEST(DebugVarsWrite, RejectsObjectAndGarbage) {
    void* slot = nullptr;
    std::string err;
    EXPECT_FALSE(writeValue("demo.Foo", &slot, "x", &err));
    EXPECT_FALSE(err.empty());

    int32_t i = 0;
    EXPECT_FALSE(writeValue("int32", &i, "not-a-number", &err));
}

// ---- evaluateCondition (CP6f) ----

namespace {
    // Build a locals vector pointing at caller-owned storage.
    DbgVar local(const char* name, const char* type, void* addr) {
        DbgVar v; v.name = name; v.type = type; v.addr = addr; return v;
    }
}

TEST(EvaluateCondition, IntegerComparisons) {
    int32_t a = 6;
    std::vector<DbgVar> locals{local("a", "int32", &a)};
    std::string err;
    EXPECT_TRUE(evaluateCondition("a == 6", locals, &err)) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_FALSE(evaluateCondition("a == 7", locals, &err));
    EXPECT_TRUE(evaluateCondition("a != 7", locals, &err));
    EXPECT_TRUE(evaluateCondition("a < 10", locals, &err));
    EXPECT_TRUE(evaluateCondition("a <= 6", locals, &err));
    EXPECT_FALSE(evaluateCondition("a > 6", locals, &err));
    EXPECT_TRUE(evaluateCondition("a >= 6", locals, &err));
}

TEST(EvaluateCondition, WhitespaceAndNegativesAndUnsigned) {
    int32_t n = -3;
    uint32_t u = 5;
    std::vector<DbgVar> locals{local("n", "int32", &n), local("u", "uint32", &u)};
    std::string err;
    EXPECT_TRUE(evaluateCondition("n<0", locals, &err)) << err;       // no spaces
    EXPECT_TRUE(evaluateCondition("  n   ==   -3 ", locals, &err));   // padded
    EXPECT_TRUE(evaluateCondition("u >= 5", locals, &err));
}

TEST(EvaluateCondition, BooleanAndFloat) {
    uint8_t flag = 1;
    double d = 2.5;
    std::vector<DbgVar> locals{local("flag", "boolean", &flag),
                               local("d", "float64", &d)};
    std::string err;
    EXPECT_TRUE(evaluateCondition("flag == true", locals, &err)) << err;
    EXPECT_FALSE(evaluateCondition("flag == false", locals, &err));
    EXPECT_TRUE(evaluateCondition("d > 2.0", locals, &err));
    EXPECT_FALSE(evaluateCondition("d < 2.0", locals, &err));
}

TEST(EvaluateCondition, MalformedStopsWithError) {
    int32_t a = 6;
    std::vector<DbgVar> locals{local("a", "int32", &a)};
    std::string err;
    // No operator -> stop (true) + error.
    EXPECT_TRUE(evaluateCondition("a", locals, &err));
    EXPECT_FALSE(err.empty());
    // Unknown local -> stop + error.
    EXPECT_TRUE(evaluateCondition("z == 1", locals, &err));
    EXPECT_FALSE(err.empty());
    // Unparseable literal -> stop + error.
    EXPECT_TRUE(evaluateCondition("a == xyz", locals, &err));
    EXPECT_FALSE(err.empty());
}

// ---- runtime chain via native builders ----

TEST(DebugVarsChain, SingleFrameLocals) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr) << "chain not clean at start";

    __cajeta_dbg_frame_enter("demo.Calc::main");
    int32_t a = 6, b = 7;
    __cajeta_dbg_local("a", "int32", &a);
    __cajeta_dbg_local("b", "int32", &b);

    std::vector<DbgFrameInfo> frames = walkFrames(*__cajeta_dbg_top_ptr());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].func, "demo.Calc::main");
    ASSERT_EQ(frames[0].locals.size(), 2u);
    EXPECT_EQ(frames[0].locals[0].name, "a");
    EXPECT_EQ(frames[0].locals[1].name, "b");
    EXPECT_EQ(formatValue(frames[0].locals[0].type, frames[0].locals[0].addr), "6");
    EXPECT_EQ(formatValue(frames[0].locals[1].type, frames[0].locals[1].addr), "7");

    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}

TEST(DebugVarsChain, NestedFramesInnermostFirst) {
    ASSERT_EQ(*__cajeta_dbg_top_ptr(), nullptr);

    __cajeta_dbg_frame_enter("demo.A::outer");
    int32_t x = 1;
    __cajeta_dbg_local("x", "int32", &x);
    __cajeta_dbg_frame_enter("demo.A::inner");
    int32_t y = 2;
    __cajeta_dbg_local("y", "int32", &y);

    std::vector<DbgFrameInfo> frames = walkFrames(*__cajeta_dbg_top_ptr());
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].func, "demo.A::inner");   // innermost first
    EXPECT_EQ(frames[1].func, "demo.A::outer");
    EXPECT_EQ(frames[0].locals[0].name, "y");
    EXPECT_EQ(frames[1].locals[0].name, "x");

    __cajeta_dbg_frame_leave();
    __cajeta_dbg_frame_leave();
    EXPECT_EQ(*__cajeta_dbg_top_ptr(), nullptr);
}
