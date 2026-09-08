// Assignment compatibility for reference-typed local declarations
// (spec Types §3.2, §3.5; Templates §11).
//
// Measured 2026-09-08 on 0.27.0: a local declaration whose initializer
// is an unrelated class type compiles — `B b = a` where `a : A` and the
// classes share no hierarchy runs without diagnostic. The same hole
// admits cross-parameterization assignment of a monomorphized template:
// `Box<float64> b = a` where `a : Box<int32>` compiles, and reading
// `b.v` reinterprets the int32 bit pattern as float64 (7 reads back as
// 3.45846e-323). The instantiations have distinct generated layouts —
// the misread is the proof — but the declaration check never compares
// the initializer's class against the declared class.
//
// Static-field initializers already have this check
// (CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH, CajetaClass.cpp); these
// tests expect the same code from the local-declaration path. Adjust
// the expected code if the fix names a dedicated one.
//
// RED until the local-declaration compatibility check lands.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

}  // namespace

// An initializer of an unrelated class type must be rejected.
TEST(AssignmentCompatibilityTests, DISABLED_unrelatedClassInitializerRejected) {
    const char* src =
        "package test;\n"
        "public class A { public A() {} }\n"
        "public class B { public B() {} }\n"
        "public final class D {\n"
        "    public static void run() {\n"
        "        A a = heap A();\n"
        "        B b = a;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH");
}

// Distinct parameterizations of one template are distinct types
// (Types §3.5): Box<int32> is not assignable to Box<float64>.
TEST(AssignmentCompatibilityTests,
     DISABLED_crossParameterizationInitializerRejected) {
    const char* src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T v;\n"
        "    public Box(T v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static void run() {\n"
        "        Box<int32> a = heap Box<int32>(7);\n"
        "        Box<float64> b = a;\n"
        "        float64 x = b.v;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH");
}
