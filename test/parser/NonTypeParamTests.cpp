//
// Non-type (integer-constant) template PARAMETER declarations:
// `class Foo<T, uint32 N>`. The non-type-argument substrate (CajetaConstantType
// + the integer-literal arm in fromContext) already let `Vector<T, N>` *consume*
// an integer argument; these tests cover *declaring* a user/template class that
// takes one — the grammar `typeParameter` non-type alt (`primitiveType
// identifier`), the TypeParameter.isNonType plumbing, and the instantiator's
// parameter-kind check. This is the substrate the cooperative-matrix surface
// (`CooperativeMatrix<T, uint32 Rows, uint32 Cols, uint32 Use>`) builds on; the
// integer params are carried in the type (for the device lowerer to read) and
// need not be referenced in the class body.
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

// A user template that declares a non-type integer parameter alongside a type
// parameter instantiates and runs. N is carried in the type (unused in the
// body), exactly as CooperativeMatrix will carry rows/cols/use.
TEST(NonTypeParamTests, userTemplateWithIntegerParamInstantiates) {
    auto src =
        "package test;\n"
        "public class Tile<T, uint32 N> {\n"
        "    public T through(T value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tile<int32, 16> t = new Tile<int32, 16>();\n"
        "        return t.through(42);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Distinct N values are distinct instantiations of the same template; both lay
// out and run (the integer argument participates in the cache key).
TEST(NonTypeParamTests, distinctIntegerArgsAreDistinctInstantiations) {
    auto src =
        "package test;\n"
        "public class Tile<T, uint32 N> {\n"
        "    public T through(T value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tile<int32, 8>  a = new Tile<int32, 8>();\n"
        "        Tile<int32, 16> b = new Tile<int32, 16>();\n"
        "        return a.through(1) + b.through(2);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// Kind mismatch: a non-type parameter requires an integer-constant argument; a
// type argument in that slot is a clean error (CAJETA_ERROR_TYPE_PARAMETER_KIND).
TEST(NonTypeParamTests, typeArgForNonTypeParamRejected) {
    auto src =
        "package test;\n"
        "public class Tile<T, uint32 N> {\n"
        "    public T through(T value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tile<int32, float32> t = new Tile<int32, float32>();\n"
        "        return t.through(1);\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// A non-type parameter used as an identifier inside a NESTED type argument in a
// method SIGNATURE (parameter/return type) substitutes correctly during
// monomorphization — the param name resolves through the substitution stack
// (CajetaType.cpp lookupTypeParameter) to its CajetaConstantType, exactly like a
// type parameter. This is what lets CooperativeMatrix's `mma` take
// `CooperativeMatrix<T, Rows, Cols, 0>`-typed operands. (Substituting a non-type
// param inside a `new Inner<T, N>()` NewExpression in a body is a separate path
// that is NOT yet wired — CooperativeMatrix's placeholder bodies don't need it.)
TEST(NonTypeParamTests, nonTypeParamSubstitutesInNestedTypeArg) {
    auto src =
        "package test;\n"
        "public class Inner<T, uint32 N> {\n"
        "    public int32 tag() { return 7; }\n"
        "}\n"
        "public class Outer<T, uint32 N> {\n"
        "    public int32 take(Inner<T, N> x) { return x.tag(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Outer<int32, 16> o = new Outer<int32, 16>();\n"
        "        Inner<int32, 16> i = new Inner<int32, 16>();\n"
        "        return o.take(i);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Kind mismatch the other way: a type parameter cannot take an integer-constant
// argument.
TEST(NonTypeParamTests, constantArgForTypeParamRejected) {
    auto src =
        "package test;\n"
        "public class Tile<T, uint32 N> {\n"
        "    public T through(T value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tile<4, 16> t = new Tile<4, 16>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}
