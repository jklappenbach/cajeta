// Abstract classes and abstract methods (spec Classes §8.5). Three checks are
// missing, so each test here is RED until one lands: allocating a class with
// an unimplemented abstract method, an abstract method declared with a body,
// and a class declaring an abstract method without the `abstract` modifier.

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
