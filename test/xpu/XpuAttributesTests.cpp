//
// Step 1 — XPU annotation recognition.
//
// Verifies that @Kernel / @Device / @Host / @Wave on
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
#include <optional>
#include <utility>
#include <fstream>
#include <random>
#include <string>
#include "KernelLoweringProbe.h"

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
using cajeta::xpu::probe::compileForInspection;


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

namespace {

// The XpuKernelAttr of `run` declared as `@Kernel(<args>)`.
std::optional<XpuKernelAttr> kernelWith(const std::string& args) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel(" + args + ") public static void run() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    if (!klass) return std::nullopt;
    auto run = findMethod(klass, "run");
    if (!run) return std::nullopt;
    return XpuKernelAttr::from(*run);
}

// The backends `attr` emits for, in a fixed order, as one string.
std::string emitted(const XpuKernelAttr& attr) {
    std::string out;
    for (auto [b, n] : {std::pair{XpuBackend::Cpu, "cpu"}, {XpuBackend::Vulkan, "vulkan"},
                        {XpuBackend::Amd, "amdgpu"}, {XpuBackend::Nvidia, "nvptx"}})
        if (attr.emitsFor(b)) out += std::string(out.empty() ? "" : " ") + n;
    return out;
}

} // namespace

// xpu-kernel-targets 1.1.1 (spec 2.1, 2.4): "gpu" is every GPU backend,
// "cpu" the cpu backend alone, exact names exactly those.
TEST(XpuAttributesTests, targetsNameTheBackendsAKernelIsBuiltFor) {
    for (auto [args, want] : {std::pair{"targets = {\"gpu\"}", "vulkan amdgpu nvptx"},
                              {"targets = {\"cpu\"}", "cpu"},
                              {"targets = \"cpu\"", "cpu"},
                              {"targets = {\"vulkan\", \"amdgpu\"}", "vulkan amdgpu"},
                              {"targets = {\"cpu\", \"nvptx\"}", "cpu nvptx"}}) {
        auto attr = kernelWith(args);
        ASSERT_TRUE(attr.has_value()) << args;
        EXPECT_EQ(emitted(*attr), want) << args;
        EXPECT_EQ(attr->targetsError(), "") << args;
    }
}

// 1.1.1 (spec 2.2): no targets is every backend, the cpu included.
TEST(XpuAttributesTests, noTargetsMeansEveryBackend) {
    auto attr = kernelWith("");
    ASSERT_TRUE(attr.has_value());
    EXPECT_EQ(emitted(*attr), "cpu vulkan amdgpu nvptx");
}

// 1.1.3: the aliases @Unlowered accepts name the same backends.
TEST(XpuAttributesTests, targetAliasesCanonicaliseAsUnloweredsDo) {
    for (auto [args, want] : {std::pair{"targets = {\"cuda\"}", "nvptx"},
                              {"targets = {\"hip\"}", "amdgpu"},
                              {"targets = {\"spirv\"}", "vulkan"},
                              {"targets = {\"GPU\"}", "vulkan amdgpu nvptx"}}) {
        auto attr = kernelWith(args);
        ASSERT_TRUE(attr.has_value()) << args;
        EXPECT_EQ(emitted(*attr), want) << args;
    }
}

// 1.1.2 (spec 2.3): a name that is no backend is recorded, naming it, for
// the build to refuse.
TEST(XpuAttributesTests, anUnknownTargetIsRecordedByName) {
    auto attr = kernelWith("targets = {\"gpu\", \"tpu\"}");
    ASSERT_TRUE(attr.has_value());
    EXPECT_NE(attr->targetsError().find("'tpu'"), std::string::npos) << attr->targetsError();
}

// 1.1.4 (spec 2.7): every instantiation of a templated kernel inherits its targets.
TEST(XpuAttributesTests, everyInstantiationInheritsItsKernelsTargets) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Q<uint32 N> {\n"
        "    public Q() { return; }\n"
        "    @Kernel(targets = {\"gpu\"})\n"
        "    public static void k(KernelBuffer<uint32> out) { out[KernelThread.globalIdX()] = N; }\n"
        "    public void run(KernelBuffer<uint32> out) {\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        k.launch(s, grid: [1], block: [32])(out);\n"
        "        s.sync();\n"
        "    }\n"
        "}\n"
        "public class M {\n"
        "    public static int32 run() {\n"
        "        KernelBuffer<uint32> b = heap KernelBuffer<uint32>(0, 32);\n"
        "        Q<4> a #= heap Q<4>();\n"
        "        a.run(b);\n"
        "        Q<8> c #= heap Q<8>();\n"
        "        c.run(b);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    int seen = 0;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon.rfind("test.Q<", 0) != 0 || !klass) continue;
        auto k = findMethod(klass, "k");
        ASSERT_NE(k, nullptr) << canon;
        auto attr = XpuKernelAttr::from(*k);
        ASSERT_TRUE(attr.has_value()) << canon;
        EXPECT_EQ(emitted(*attr), "vulkan amdgpu nvptx") << canon;
        ++seen;
    }
    EXPECT_EQ(seen, 2);
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

// parseBackend round-trips backendName, which answers the registrations' names.
TEST(XpuAttributesTests, backendNameRoundTrip) {
    EXPECT_EQ(*cajeta::xpu::parseBackend("nvptx"),  XpuBackend::Nvidia);
    EXPECT_EQ(*cajeta::xpu::parseBackend("AMDGPU"), XpuBackend::Amd);
    EXPECT_EQ(*cajeta::xpu::parseBackend("Vulkan"), XpuBackend::Vulkan);
    EXPECT_EQ(*cajeta::xpu::parseBackend("cpu"),    XpuBackend::Cpu);
    EXPECT_FALSE(cajeta::xpu::parseBackend("metal").has_value());

    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Nvidia), "nvptx");
    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Amd),    "amdgpu");
    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Vulkan), "vulkan");
    EXPECT_STREQ(cajeta::xpu::backendName(XpuBackend::Cpu),    "cpu");
}
