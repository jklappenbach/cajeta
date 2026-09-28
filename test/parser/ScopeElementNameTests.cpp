// `scope` is a keyword for the structured-concurrency block, and an annotation
// element name must also accept it (component-scopes spec 1.3.9).

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/Annotatable.h"
#include "../xpu/KernelLoweringProbe.h"

#include <string>

using cajeta::Compiler;
using cajeta::AnnotationArgKind;
using cajeta::xpu::probe::compileForInspection;

// plan 0.1.1: `scope = "X"` parses as a named String argument.
TEST(ScopeElementNameTests, scopeIsAnElementName) {
    auto src =
        "package test;\n"
        "@Component(scope = \"Request\") public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    auto ann = klass->findAnnotation("Component");
    ASSERT_NE(ann, nullptr);
    auto* arg = ann->findArg("scope");
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->kind, AnnotationArgKind::String);
    EXPECT_EQ(ann->getString("scope"), "Request");
}

// plan 0.1.2: an annotation declaration may name a member `scope`.
TEST(ScopeElementNameTests, scopeIsAnAnnotationMemberName) {
    auto src =
        "package test;\n"
        "annotation Lifetime {\n"
        "    String scope() default \"\";\n"
        "}\n"
        "@Lifetime(scope = \"Session\") public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    auto ann = klass->findAnnotation("Lifetime");
    ASSERT_NE(ann, nullptr);
    EXPECT_EQ(ann->getString("scope"), "Session");
}

// plan 0.1.3: the block statement is unchanged beside the element name.
TEST(ScopeElementNameTests, scopeBlockStillParsesBesideTheName) {
    auto src =
        "package test;\n"
        "@Component(scope = \"Request\") public class D {\n"
        "    static int32 one() { return 1; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        scope {\n"
        "            r = D.one();\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    ASSERT_NE(module->getStructures()["test.D"], nullptr);
}
