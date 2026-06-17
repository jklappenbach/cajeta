//
// Annotation smoke tests. v1 scope:
//   - Class declarations with user annotations parse and codegen without
//     crashing (the annotation is captured by name on the CajetaClass via
//     Annotatable::addAnnotation; consumers can read it later).
//   - `annotation MyAnn { ... }` declarations parse without crashing.
//
// Deferred:
//   - Element-value capture (`@MyAnn(key="hello")`).
//   - Annotation-driven semantic actions beyond the existing wire-format
//     markers (@BigEndian / @LittleEndian / @Align on structs).
//   - Method / field-level annotation capture.
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

// Class with a user annotation compiles and runs normally. Behavior of the
// annotated code is unchanged — annotations are pure metadata in v1.
TEST(AnnotationTests, classWithUserAnnotationCompiles) {
    auto src =
        "package test;\n"
        "@Loggable\n"
        "public class Counter {\n"
        "    public int32 value() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        return c.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `annotation MyAnn` declaration parses without breaking compilation of
// other code in the same translation unit.
TEST(AnnotationTests, annotationTypeDeclarationParses) {
    auto src =
        "package test;\n"
        "public annotation Loggable { }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Annotation with element values parses, even though the values aren't
// captured in v1.
TEST(AnnotationTests, annotationWithElementValuesParses) {
    auto src =
        "package test;\n"
        "@Loggable(level=\"info\")\n"
        "public class Service {\n"
        "    public int32 ping() { return 200; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Service s = heap Service();\n"
        "        return s.ping();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 200);
}
