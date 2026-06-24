//
// Step 1 — XPU annotation recognition.
//
// Verifies that @Kernel / @Device / @Host / @Wave / @Backend on
// method declarations are captured by the annotation system (no
// new visitor work required — they ride the same general path that
// already handles @SuppressLint / @Component / @Native) and that
// XpuKernelAttr's typed view extracts the co-annotation values
// correctly.
//
// Pattern mirrors test/parser/AnnotationParsingTests.cpp — direct
// Compiler use without the JIT helper, so we can inspect Annotatable
// state on parsed methods.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/method/Method.h"

#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/core/XpuKernelAttr.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::AnnotationArgKind;
using cajeta::xpu::XpuAttr;
using cajeta::xpu::XpuBackend;
using cajeta::xpu::XpuKernelAttr;

namespace {

// Local re-roll of the test/parser/AnnotationParsingTests helper.
// The shared one lives in an anonymous namespace there; per the
// in-file comment, the convention is for each test file to roll its
// own rather than expose. Writes the source to a fresh temp directory
// matching the fqClassName layout, compiles via a fresh Compiler,
// returns the compiled module so the test can inspect structures.
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_attr_" + std::to_string(rng()));
    std::filesystem::create_directories(base);

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
                 / ("cajeta_xpu_attr_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == name) return m;
    }
    return nullptr;
}

} // namespace

// @Kernel is captured as a bare annotation (no args). findAnnotation
// returns the instance; isKernel() helper returns true. Absence of
// args is fine — the instance is recorded for its name alone.
TEST(XpuAttributesTests, kernelAnnotationRecognizedOnMethod) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    EXPECT_NE(run->findAnnotation(XpuAttr::Kernel), nullptr);
    EXPECT_TRUE(cajeta::xpu::isKernel(*run));
    EXPECT_FALSE(cajeta::xpu::isDevice(*run));
    EXPECT_FALSE(cajeta::xpu::isHost(*run));
}

// @Device and @Host are independently recognized. Multiple XPU
// function-attributes on one method are allowed (e.g. `@Host @Device`
// for dual-emit) — both predicates report true.
TEST(XpuAttributesTests, deviceAndHostRecognizedIndependently) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Device public static void d() { }\n"
        "    @Host public static void h() { }\n"
        "    @Host @Device public static void dual() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);

    auto d = findMethod(klass, "d");
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(cajeta::xpu::isDevice(*d));
    EXPECT_FALSE(cajeta::xpu::isHost(*d));
    EXPECT_FALSE(cajeta::xpu::isKernel(*d));

    auto h = findMethod(klass, "h");
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(cajeta::xpu::isHost(*h));
    EXPECT_FALSE(cajeta::xpu::isDevice(*h));

    auto dual = findMethod(klass, "dual");
    ASSERT_NE(dual, nullptr);
    EXPECT_TRUE(cajeta::xpu::isHost(*dual));
    EXPECT_TRUE(cajeta::xpu::isDevice(*dual));
}

// @Wave(width = 32) captures as a named Int64 arg. XpuKernelAttr
// surfaces it via waveWidth().
TEST(XpuAttributesTests, waveWidthExtractedFromKernelAttr) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel @Wave(width = 32) public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto attr = XpuKernelAttr::from(*run);
    ASSERT_TRUE(attr.has_value());
    ASSERT_TRUE(attr->waveWidth().has_value());
    EXPECT_EQ(*attr->waveWidth(), 32);
}

// Grammar alias: @Wave(width: 32) parses identically to
// @Wave(width = 32). The CajetaXPU.md spec uses `:`; the rest of the
// codebase uses `=`. elementValuePair accepts both since step 1b.
TEST(XpuAttributesTests, waveWidthAcceptsColonSyntax) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel @Wave(width: 32) public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto attr = XpuKernelAttr::from(*run);
    ASSERT_TRUE(attr.has_value());
    ASSERT_TRUE(attr->waveWidth().has_value());
    EXPECT_EQ(*attr->waveWidth(), 32);
}

// XpuKernelAttr::from returns nullopt when @Kernel is absent, even if
// other XPU co-annotations are present. The view is per-kernel; @Wave
// without @Kernel is meaningless.
TEST(XpuAttributesTests, fromReturnsNulloptWhenKernelAbsent) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Wave(width = 32) public static void notAKernel() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "notAKernel");
    ASSERT_NE(run, nullptr);

    EXPECT_FALSE(XpuKernelAttr::from(*run).has_value());
}

// @Backend("nvidia") — single unnamed-string form. Backends vector
// has one entry; emitsFor(Nvidia) true, others false.
TEST(XpuAttributesTests, singleBackendRestriction) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel @Backend(\"nvidia\") public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto attr = XpuKernelAttr::from(*run);
    ASSERT_TRUE(attr.has_value());
    ASSERT_EQ(attr->backends().size(), 1u);
    EXPECT_EQ(attr->backends()[0], XpuBackend::Nvidia);
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Nvidia));
    EXPECT_FALSE(attr->emitsFor(XpuBackend::Amd));
    EXPECT_FALSE(attr->emitsFor(XpuBackend::Vulkan));
}

// @Backend({"nvidia", "amd"}) — StringList form. Backends vector
// has both; emitsFor returns true for either and false for vulkan.
TEST(XpuAttributesTests, multiBackendRestriction) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel @Backend({\"nvidia\", \"amd\"}) public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto attr = XpuKernelAttr::from(*run);
    ASSERT_TRUE(attr.has_value());
    ASSERT_EQ(attr->backends().size(), 2u);
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Nvidia));
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Amd));
    EXPECT_FALSE(attr->emitsFor(XpuBackend::Vulkan));
}

// No @Backend annotation → emit for every configured backend. The
// emitsFor predicate returns true for all three.
TEST(XpuAttributesTests, noBackendMeansAllBackends) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto attr = XpuKernelAttr::from(*run);
    ASSERT_TRUE(attr.has_value());
    EXPECT_TRUE(attr->backends().empty());
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Nvidia));
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Amd));
    EXPECT_TRUE(attr->emitsFor(XpuBackend::Vulkan));
}

// gfx §4.a — graphics shader-stage annotations (@Vertex / @Fragment / …) are
// captured on the same general annotation path as @Kernel; the per-stage and
// isGraphicsShader predicates report correctly and don't collide with @Kernel.
TEST(XpuAttributesTests, graphicsStageAnnotationsRecognized) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex public static void vs() { }\n"
        "    @Fragment public static void fs() { }\n"
        "    @Kernel public static void cs() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.S");
    auto klass = module->getStructures()["test.S"];
    ASSERT_NE(klass, nullptr);

    auto vs = findMethod(klass, "vs");
    ASSERT_NE(vs, nullptr);
    EXPECT_TRUE(cajeta::xpu::isVertex(*vs));
    EXPECT_FALSE(cajeta::xpu::isFragment(*vs));
    EXPECT_TRUE(cajeta::xpu::isGraphicsShader(*vs));
    EXPECT_EQ(cajeta::xpu::graphicsStageCount(*vs), 1);
    EXPECT_FALSE(cajeta::xpu::isKernel(*vs));
    EXPECT_FALSE(cajeta::xpu::hasShaderStageConflict(*vs));

    auto fs = findMethod(klass, "fs");
    ASSERT_NE(fs, nullptr);
    EXPECT_TRUE(cajeta::xpu::isFragment(*fs));
    EXPECT_TRUE(cajeta::xpu::isGraphicsShader(*fs));

    auto cs = findMethod(klass, "cs");
    ASSERT_NE(cs, nullptr);
    EXPECT_TRUE(cajeta::xpu::isKernel(*cs));
    EXPECT_FALSE(cajeta::xpu::isGraphicsShader(*cs));
    EXPECT_EQ(cajeta::xpu::graphicsStageCount(*cs), 0);
}

// gfx §4.a — stage-shape validation: more than one graphics stage, or a graphics
// stage mixed with @Kernel, is a conflict; a single graphics stage is clean.
TEST(XpuAttributesTests, shaderStageConflictDetected) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex @Fragment public static void twoStages() { }\n"
        "    @Vertex @Kernel public static void mixed() { }\n"
        "    @Vertex public static void clean() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.S");
    auto klass = module->getStructures()["test.S"];
    ASSERT_NE(klass, nullptr);

    auto twoStages = findMethod(klass, "twoStages");
    ASSERT_NE(twoStages, nullptr);
    EXPECT_EQ(cajeta::xpu::graphicsStageCount(*twoStages), 2);
    EXPECT_TRUE(cajeta::xpu::hasShaderStageConflict(*twoStages));

    auto mixed = findMethod(klass, "mixed");
    ASSERT_NE(mixed, nullptr);
    EXPECT_TRUE(cajeta::xpu::hasShaderStageConflict(*mixed));

    auto clean = findMethod(klass, "clean");
    ASSERT_NE(clean, nullptr);
    EXPECT_FALSE(cajeta::xpu::hasShaderStageConflict(*clean));
}

// parseBackend round-trips backendName.
TEST(XpuAttributesTests, backendNameRoundTrip) {
    EXPECT_EQ(*cajeta::xpu::parseBackend("nvidia"), XpuBackend::Nvidia);
    EXPECT_EQ(*cajeta::xpu::parseBackend("AMD"),    XpuBackend::Amd);
    EXPECT_EQ(*cajeta::xpu::parseBackend("Vulkan"), XpuBackend::Vulkan);
    EXPECT_FALSE(cajeta::xpu::parseBackend("metal").has_value());

    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Nvidia), "nvidia");
    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Amd),    "amd");
    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Vulkan), "vulkan");
}
