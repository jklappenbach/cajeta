//
// Sealed class smoke tests. v1: parses the SEALED / NON_SEALED modifiers
// and the `permits` clause, no semantic check that subclasses appear in
// the permits list. Java 17 sealed-class enforcement is deferred.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A sealed class with a permits clause parses + runs. The permits clause
// is captured by the grammar but ignored for v1 (no compile-time check
// that subclasses appear in the list).
TEST(SealedClassTests, sealedWithPermitsParses) {
    auto src =
        "package test;\n"
        "public sealed class Shape permits Circle, Square {\n"
        "    public int32 sides() { return 0; }\n"
        "}\n"
        "public final class Circle extends Shape {\n"
        "    public int32 sides() { return 1; }\n"
        "}\n"
        "public final class Square extends Shape {\n"
        "    public int32 sides() { return 4; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Square s = new Square();\n"
        "        return s.sides();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// `non-sealed` modifier on a subclass of a sealed type also parses.
TEST(SealedClassTests, nonSealedModifierParses) {
    auto src =
        "package test;\n"
        "public sealed class Shape permits Triangle {\n"
        "    public int32 sides() { return 0; }\n"
        "}\n"
        "public non-sealed class Triangle extends Shape {\n"
        "    public int32 sides() { return 3; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Triangle t = new Triangle();\n"
        "        return t.sides();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}
