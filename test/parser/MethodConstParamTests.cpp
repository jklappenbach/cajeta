//
// Method-level non-type (integer-constant) template PARAMETERS:
// `public static <uint32 N> ... f(...)`, called `f<8>(...)`. The class-level
// substrate (NonTypeParamTests: TypeParameter.isNonType + the instantiator's
// parameter-kind check + CajetaConstantType arguments) already lets a CLASS
// take an integer parameter; these tests extend the same to a METHOD's own
// type-parameter list, the enabler for `KernelBuffer<T>.vload<N>() ->
// Vector<T,N>` (kernel-vector-loadstore plan, Unit 1).
//
// Mirrors MethodTemplateExplicitArgsTests (the `expr.method<Args>(...)`
// call-site form) but with an integer-constant argument in the slot, and
// NonTypeParamTests (the kind checks).
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

double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace

// 1.1.1 — a static method declaring a non-type integer parameter `<uint32 N>`
// instantiates and runs with the constant supplied explicitly at the call site.
// N is carried in the instantiation (unused in the body here), exactly as the
// class-level Tile<T,uint32 N> does.
TEST(MethodConstParamTests, methodConstParamInstantiates) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 idN<uint32 N>(int32 x) { return x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.idN<8>(42);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// 1.1.4 — distinct constant arguments are distinct instantiations (the integer
// value participates in the cache key), both lay out and run.
TEST(MethodConstParamTests, distinctConstArgsAreDistinctInstantiations) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 idN<uint32 N>(int32 x) { return x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.idN<8>(1) + Util.idN<16>(2);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 1.1.2 — kind mismatch: a non-type parameter requires an integer-constant
// argument; a TYPE argument in that slot is a clean compile error
// (CAJETA_ERROR_TYPE_PARAMETER_KIND), mirroring the class-level check.
TEST(MethodConstParamTests, typeArgForConstParamRejected) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 idN<uint32 N>(int32 x) { return x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.idN<float32>(1);\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// 1.1.3 — the const parameter `N` substitutes into a signature type that is a
// USER template's non-type argument (`Box<N>`): at the call `useN<8>(...)`, N
// binds to 8 and the parameter type resolves to `Box<8>`, matching the supplied
// `Box<8>`. Mirrors the class-level `nonTypeParamSubstitutesInNestedTypeArg`,
// now driven by a METHOD-level const param. (Note: `Vector<float64,N>` in a
// parsed signature is a separate path — Vector's resolver demands a syntactic
// integer literal for N; the kernel `vload<N>` delivers `Vector<T,N>` via its
// intrinsic result type, not a parsed signature, so it does not need this.)
TEST(MethodConstParamTests, constParamInNestedUserTemplateSig) {
    auto src =
        "package test;\n"
        "public class Box<uint32 M> {\n"
        "    public int32 tag() { return 5; }\n"
        "}\n"
        "public class Util {\n"
        "    public static int32 useN<uint32 N>(Box<N> b) { return b.tag(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<8> b = heap Box<8>();\n"
        "        return Util.useN<8>(b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 1.1.5 — a method may mix a type parameter and a const parameter `<R, uint32 N>`;
// the const composes with the ordinary type parameter.
TEST(MethodConstParamTests, mixedTypeAndConstParams) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static R firstOf<R, uint32 N>(R a, R b) { return a; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Util.firstOf<int32, 4>(7, 9);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
