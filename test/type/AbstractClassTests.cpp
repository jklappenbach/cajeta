// Abstract classes and abstract methods (spec Classes §8.5).
//
// What works today, measured 2026-09-09 on 0.27.0 with `cajeta jit-run`:
// an abstract base with a concrete subclass dispatches correctly through
// a base-typed binding, a concrete class that inherits an abstract method
// without overriding it is rejected with
// CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED, and an abstract method in one
// parent is satisfied by a concrete same-signature method in another
// (test/parser/MultiClassingPhase1Tests.cpp pins that case).
//
// Three checks are missing. Each test below is RED until one lands.
//
// 1. Allocating a class that still has an unimplemented abstract method
//    compiles. The empty vtable slot is called at run time and the
//    process takes SIGSEGV at a null fault address — measured with
//    `heap Shape()` where Shape declares `public abstract int32 area()`,
//    exit 139, "SIGSEGV caught — fault addr (nil)". The allocation site
//    is where this must be rejected.
// 2. An abstract method declared with a body is accepted and the body is
//    ignored.
// 3. A class that declares an abstract method is not required to carry
//    the `abstract` modifier, so a class that reads as concrete can hold
//    an empty slot.
//
// The expected codes below are named for what they check. Adjust them if
// the fix picks different ones.

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

// An abstract class is not instantiable. Today this compiles and the call
// through the empty slot faults at run time.
TEST(AbstractClassTests, DISABLED_abstractClassIsNotInstantiable) {
    const char* src =
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shape s = heap Shape();\n"
        "        return s.area();\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_INSTANTIATION");
}

// An abstract method declares a signature and no body.
TEST(AbstractClassTests, DISABLED_abstractMethodWithBodyRejected) {
    const char* src =
        "package test;\n"
        "public abstract class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_METHOD_HAS_BODY");
}

// A class that declares an abstract method is itself abstract, and must
// say so.
TEST(AbstractClassTests, DISABLED_abstractMethodRequiresAbstractClass) {
    const char* src =
        "package test;\n"
        "public class Shape {\n"
        "    public Shape() { return; }\n"
        "    public abstract int32 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_ABSTRACT_METHOD_IN_CONCRETE_CLASS");
}
