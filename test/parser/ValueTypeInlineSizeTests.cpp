//
// S4 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): @ValueType operators are marked
// `alwaysinline` so a small operator folds to flat, intrinsic-like IR at O0
// (the AlwaysInlinerPass runs even at O0). A SIZE GUARD removes the attribute
// from a genuinely large operator body so it keeps a real call instead of
// bloating every call site. These tests pin both halves by inspecting the
// post-AlwaysInline host IR (Options::captureIr -> getModuleIr()).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Count occurrences of a `call ... @"test.Vec2::operator+"` in the IR — i.e.
// whether the operator survived as a real (non-inlined) call.
bool hasOperatorPlusCall(const std::string& ir) {
    // A surviving call reads e.g.  call <ty> @"test.Vec2::operator+(...)"
    size_t pos = 0;
    while ((pos = ir.find("@\"test.Vec2::operator+", pos)) != std::string::npos) {
        // Look back on the line for a `call` keyword (defining `define ...` is
        // the body itself, which is fine — the body always exists; we only care
        // whether a CALL site to it remains).
        size_t lineStart = ir.rfind('\n', pos);
        std::string line = ir.substr(lineStart == std::string::npos ? 0 : lineStart,
                                     pos - (lineStart == std::string::npos ? 0 : lineStart));
        if (line.find("call ") != std::string::npos) return true;
        pos += 1;
    }
    return false;
}

std::string moduleIrFor(const std::string& opPlusBody) {
    std::string src =
        "package test;\n"
        "@ValueType public final class Vec2 {\n"
        "    public float32 x;\n"
        "    public float32 y;\n"
        "    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }\n"
        "    public static Vec2 operator+ (Vec2 a, Vec2 b) {\n"
        + opPlusBody +
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vec2 a = stack Vec2(1.0f, 2.0f);\n"
        "        Vec2 b = stack Vec2(3.0f, 4.0f);\n"
        "        Vec2 c = a + b;\n"
        "        return (int32)(c.x + c.y);\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    return jit->getModuleIr();
}

} // namespace

// A small @ValueType operator+ is force-inlined: after the O0 AlwaysInlinerPass
// there is no surviving call to test.Vec2::operator+ in the module.
TEST(ValueTypeInlineSizeTests, smallOperatorFoldsAtO0) {
    std::string ir = moduleIrFor(
        "        return stack Vec2(a.x + b.x, a.y + b.y);\n");
    ASSERT_FALSE(ir.empty());
    EXPECT_FALSE(hasOperatorPlusCall(ir))
        << "small @ValueType operator+ should have folded (no surviving call)\n"
        << ir;
}

// A deliberately LARGE operator+ body trips the size guard: the alwaysinline
// attribute is removed, so a real call survives at O0.
TEST(ValueTypeInlineSizeTests, largeOperatorKeepsCall) {
    // Build a long straight-line body (well over the ~100-instruction guard):
    // each line is ~6 IR instructions (two loads, fmul, load, fadd, store).
    std::string body = "        float32 acc = a.x;\n";
    for (int i = 0; i < 60; ++i) {
        body += "        acc = acc * a.x + b.y;\n";
    }
    body += "        return stack Vec2(acc, a.y + b.y);\n";

    std::string ir = moduleIrFor(body);
    ASSERT_FALSE(ir.empty());
    EXPECT_TRUE(hasOperatorPlusCall(ir))
        << "large @ValueType operator+ should keep a real call (size guard)\n"
        << ir;
}
