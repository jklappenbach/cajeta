//
// Default type arguments on a class type parameter: `class Box<T = float32>`.
// A trailing parameter may declare a default `typeType`; an instantiation that
// omits it (bare `Box`, `Box<>`-free construction, or fewer args than params)
// fills the default, so `Box` and `Box<float32>` are the SAME type. This is the
// substrate that lets `Texture2D<T = float32>` keep every existing bare
// `Texture2D` spelling working while gaining a texel-type parameter.
//
// Grammar: the `(ASSIGN typeType)?` tail on the typeParameter type-arm.
// Plumbing: TypeParameter.defaultType (captured at both parse sites), the
// trailing-default fill in CajetaClass::instantiateInternal (before the cache
// key + arity check, so the canonical name and cached instantiation are
// shared), and the bare-reference resolution in CajetaType::fromContext +
// NewExpression (type-use and construction sites).
//

#include <gtest/gtest.h>
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

// Bare construction of a default-bearing template fills the default: `heap
// Box(42)` for `class Box<T = int32>` allocates a `Box<int32>`, so the i32
// round-trips through the `T value` field. This is the `heap Texture2D(w, h)`
// ergonomic the default exists to preserve.
TEST(DefaultTypeParamTests, bareConstructionResolvesToDefault) {
    auto src =
        "package test;\n"
        "public class Box<T = int32> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(42);\n"          // bare Box == Box<int32>
        "        return b.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// An explicit type argument overrides the default — `Box<int64>` stores/returns
// an i64, distinct from the defaulted `Box<int32>`.
TEST(DefaultTypeParamTests, explicitArgOverridesDefault) {
    auto src =
        "package test;\n"
        "public class Box<T = int32> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int64> b = heap Box<int64>(7);\n"
        "        return (int32) b.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Type identity: a bare `Box` and an explicit `Box<int32>` are the SAME
// instantiation. A method whose parameter is typed bare `Box` accepts a
// `Box<int32>` argument (the call only resolves if the types are identical),
// and the value flows through unchanged.
TEST(DefaultTypeParamTests, bareEqualsExplicitDefaultIdentity) {
    auto src =
        "package test;\n"
        "public class Box<T = int32> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 take(Box b) { return b.get(); }\n"
        "    public static int32 run() {\n"
        "        Box<int32> x = heap Box<int32>(5);\n"
        "        return take(x);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// A trailing default is filled when only the leading argument is supplied:
// `Pair<float32>` for `class Pair<A, B = int32>` is `Pair<float32, int32>`.
TEST(DefaultTypeParamTests, trailingDefaultPartialFill) {
    auto src =
        "package test;\n"
        "public class Pair<A, B = int32> {\n"
        "    A a;\n"
        "    B b;\n"
        "    public Pair(A a, B b) { this.a = a; this.b = b; }\n"
        "    public B second() { return this.b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair<float32> p = heap Pair<float32>(1.5f, 9);\n"
        "        return p.second();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// Several trailing defaults fill at once: `Triple<float32>` for
// `class Triple<A, B = int32, C = int64>` is `Triple<float32, int32, int64>` —
// both B and C come from their defaults. Confirms the fill walks every omitted
// trailing parameter, not just the last one.
TEST(DefaultTypeParamTests, multipleTrailingDefaultsFill) {
    auto src =
        "package test;\n"
        "public class Triple<A, B = int32, C = int64> {\n"
        "    A a;\n"
        "    B b;\n"
        "    C c;\n"
        "    public Triple(A a, B b, C c) { this.a = a; this.b = b; this.c = c; }\n"
        "    public int32 sum() { return (int32) this.b + (int32) this.c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Triple<float32> t = heap Triple<float32>(1.0f, 10, 20);\n"
        "        return t.sum();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}
