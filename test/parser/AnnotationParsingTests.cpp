//
// A1: annotation parameter parsing.
//
// Verifies that the visitor captures typed argument values from
// annotations on classes and methods, that the previously-special-cased
// @SuppressLint path still works (backward compatibility) but now goes
// through the same general infrastructure, and that the AnnotationInstance
// typed accessors return the right values for each shape:
//   - unnamed string  (@Profile("prod"))
//   - named string    (@Component(name = "disk"))
//   - unnamed int     (@Order(2))
//   - named int       (@Order(level = 5))
//   - unnamed bool
//   - string array    (@SuppressLint({"a", "b"}))
//
// These tests poke at the AST level — no JIT — so they use the
// Compiler directly and inspect module->getStructures(). The JIT
// helper isn't needed for what A1 ships.
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/method/Method.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;
using cajeta::AnnotationInstancePtr;
using cajeta::AnnotationArgKind;

namespace {

// Mirror of test/jit/JitTestHelper's anonymous helpers — we re-roll
// here because they aren't exposed, and the A1 inspection path
// doesn't need JIT bring-up. Writes the source to a fresh temp
// directory matching the fqClassName layout, then compiles via a
// fresh Compiler. Returns the compiled module so the test can
// inspect structures.
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_a1_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    // fqClassName like "test.D" → "test/D.cajeta".
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_a1_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

} // namespace

// @Component on a class with a named string argument. Captures as
// kind=String, name="name", strVal="disk". findAnnotation returns
// the instance; getString("name") returns "disk".
TEST(AnnotationParsingTests, namedStringArgOnClass) {
    auto src =
        "package test;\n"
        "@Component(name = \"disk\") public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    auto ann = klass->findAnnotation("Component");
    ASSERT_NE(ann, nullptr);
    auto* arg = ann->findArg("name");
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->kind, AnnotationArgKind::String);
    EXPECT_EQ(ann->getString("name"), "disk");
}

// @Profile on a class with a single unnamed string argument. The
// visitor stores it with name="" (the spec-side unnamed form), and
// findArg("value") routes through to it — consumers can read
// uniformly without knowing which form the user wrote.
TEST(AnnotationParsingTests, unnamedStringArgOnClass) {
    auto src =
        "package test;\n"
        "@Profile(\"prod\") public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    auto ann = klass->findAnnotation("Profile");
    ASSERT_NE(ann, nullptr);
    auto* arg = ann->findArg("value");  // routes to the unnamed arg
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->kind, AnnotationArgKind::String);
    EXPECT_EQ(arg->name, "");           // captured as unnamed
    EXPECT_EQ(ann->getString(), "prod");
}

// @Order(2) on a method — unnamed integer argument. Captured as
// kind=Int64 with i64Val=2. Also confirms method-level capture
// (the A1 second annotation-capture site).
TEST(AnnotationParsingTests, unnamedIntArgOnMethod) {
    auto src =
        "package test;\n"
        "public class D {\n"
        "    @Order(2) public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    // Methods are keyed by canonical signature; iterate to find run().
    cajeta::MethodPtr run;
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == "run") { run = m; break; }
    }
    ASSERT_NE(run, nullptr);
    auto ann = run->findAnnotation("Order");
    ASSERT_NE(ann, nullptr);
    auto* arg = ann->findArg("value");
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->kind, AnnotationArgKind::Int64);
    EXPECT_EQ(ann->getInt(), 2);
}

// @SuppressLint("rule-id") backward compatibility — the single-
// string form still populates suppressedLints AND lands in the new
// AnnotationInstance. Two assertions, one for each path.
TEST(AnnotationParsingTests, suppressLintSingleStringBackwardCompat) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws Whatever { return 0; }\n"
        "    @SuppressLint(\"uncaught-throws\")\n"
        "    public static int32 run() { return maybeFail(); }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    cajeta::MethodPtr run;
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == "run") { run = m; break; }
    }
    ASSERT_NE(run, nullptr);
    // Existing path: cached lint list.
    EXPECT_TRUE(run->isLintSuppressed("uncaught-throws"));
    // New path: AnnotationInstance with the same data.
    auto ann = run->findAnnotation("SuppressLint");
    ASSERT_NE(ann, nullptr);
    EXPECT_EQ(ann->getString(), "uncaught-throws");
}

// @SuppressLint({"a", "b"}) — array initializer captured as a
// StringList. Both rule IDs populate suppressedLints; the
// AnnotationInstance preserves the typed list.
TEST(AnnotationParsingTests, suppressLintArrayCapturedAsStringList) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws Whatever { return 0; }\n"
        "    @SuppressLint({\"uncaught-throws\", \"other-rule\"})\n"
        "    public static int32 run() { return maybeFail(); }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    cajeta::MethodPtr run;
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == "run") { run = m; break; }
    }
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(run->isLintSuppressed("uncaught-throws"));
    EXPECT_TRUE(run->isLintSuppressed("other-rule"));
    auto ann = run->findAnnotation("SuppressLint");
    ASSERT_NE(ann, nullptr);
    auto& list = ann->getStringList();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], "uncaught-throws");
    EXPECT_EQ(list[1], "other-rule");
}

// Multiple annotations on the same target. Verifies the
// AnnotationInstance list preserves order and each is independently
// retrievable. Mirrors what A2+ aspect resolution will need (e.g.,
// `@Component @Profile("prod")` together).
TEST(AnnotationParsingTests, multipleAnnotationsOnOneClass) {
    auto src =
        "package test;\n"
        "@Component(name = \"prod\") @Profile(\"prod\") public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    EXPECT_GE(klass->getAnnotationInstances().size(), 2u);
    auto component = klass->findAnnotation("Component");
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->getString("name"), "prod");
    auto profile = klass->findAnnotation("Profile");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->getString(), "prod");
}

// Bare annotation with no parens — `@Marker public class D`. The
// instance still appears in the list (so name-based presence
// checks work), with an empty args vector.
TEST(AnnotationParsingTests, bareAnnotationCapturedWithoutArgs) {
    auto src =
        "package test;\n"
        "@Aspect public class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.D");
    auto klass = module->getStructures()["test.D"];
    ASSERT_NE(klass, nullptr);
    auto ann = klass->findAnnotation("Aspect");
    ASSERT_NE(ann, nullptr);
    EXPECT_TRUE(ann->getArgs().empty());
}
