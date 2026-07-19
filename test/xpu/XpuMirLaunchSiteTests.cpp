//
// CajetaXPU step 8 (increment B) — launch-site recognition + MIR
// body-op population.
//
// XpuMirBuilder now does two things beyond the step-4 envelope:
//   1. collectBodyOps — walks a @Kernel body for leaf builtins
//      (KernelThread / Workgroup / Barrier calls) and records them as
//      XpuMirOp leaves in XpuMirKernel::bodyOps.
//   2. launch-site recognition — walks host method bodies for the
//      `kernel.launch(stream, grid: [...], block: [...])(args)` shape
//      and records an XpuMirLaunchSite (kernel canonical + stream +
//      grid/block dim exprs + kernel args).
//
// These run on the parsed AST — no codegen — so the launch
// CallExpression (whose generateCode is still "not implemented") is
// never lowered. compileForInspection runs the parser only (see the
// XpuKernelArgTests note), which is exactly what we need.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"

#include "cajeta/xpu/mir/XpuMir.h"
#include "cajeta/xpu/mir/XpuMirBuilder.h"
#include "cajeta/xpu/mir/XpuMirOp.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using namespace cajeta::xpu::mir;

namespace {

// Parse-only compile (no Phase-2 codegen) — same pattern as the other
// XPU inspection tests. Leaves the AST built so the MIR builder can
// walk method bodies.
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_launch_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_launch_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

size_t countOps(const std::vector<XpuMirOpPtr>& ops, OpKind kind, Axis axis) {
    size_t n = 0;
    for (auto& op : ops) {
        if (op && op->kind == kind && op->axis == axis) ++n;
    }
    return n;
}

XpuMirKernelPtr findKernel(const XpuMirModulePtr& m, const std::string& suffix) {
    for (auto& k : m->kernels) {
        if (k && k->canonicalName.size() >= suffix.size() &&
            k->canonicalName.compare(k->canonicalName.size() - suffix.size(),
                                     suffix.size(), suffix) == 0) {
            return k;
        }
    }
    return nullptr;
}

} // namespace

// A @Kernel body that reads thread coordinates produces ThreadId leaf
// ops in bodyOps (globalIdX → leaf ThreadId{X}; the device pass later
// composes ctaid*ntid+tid).
TEST(XpuMirLaunchSiteTests, kernelBodyOpsCaptureThreadReads) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    ASSERT_NE(mir, nullptr);

    auto k = findKernel(mir, ".saxpy");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::ThreadId, Axis::X), 1u);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::ThreadId, Axis::Y), 0u);
}

// Workgroup and Barrier builtins are likewise recognized.
TEST(XpuMirLaunchSiteTests, kernelBodyOpsCaptureWorkgroupAndBarrier) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.Workgroup;\n"
        "import cajeta.xpu.Barrier;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k() {\n"
        "        uint32 b = Workgroup.x();\n"
        "        uint32 d = Workgroup.dimY();\n"
        "        Barrier.workgroup();\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    auto k = findKernel(mir, ".k");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::WorkgroupId, Axis::X), 1u);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::WorkgroupDim, Axis::Y), 1u);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::BarrierWorkgroup, Axis::X), 1u);
}

// A non-kernel method with no builtins yields no ops (regression guard
// that the walker doesn't manufacture spurious leaves).
TEST(XpuMirLaunchSiteTests, plainKernelHasNoBodyOps) {
    auto src =
        "package test;\n"
        "public class M {\n"
        "    @Kernel public static void run(int32 a) { int32 b = a; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    auto k = findKernel(mir, ".run");
    ASSERT_NE(k, nullptr);
    EXPECT_TRUE(k->bodyOps.empty());
}

// The launch form is recognized in a host method body: one launch site,
// with the target kernel resolved, stream captured, grid/block dims
// flattened, and the trailing (args) captured.
TEST(XpuMirLaunchSiteTests, launchSiteRecognizedInHostBody) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                              float32 a, uint32 n) { }\n"
        "    public static void run(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                           uint32 n) {\n"
        "        saxpy.launch(KernelStream.current(),\n"
        "                     grid: [(n + 255) / 256], block: [256])"
        "(y, x, 2.0f, n);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    ASSERT_NE(mir, nullptr);

    ASSERT_EQ(mir->launchSites.size(), 1u);
    auto& site = mir->launchSites[0];
    // Receiver `saxpy` resolved to the kernel canonical.
    EXPECT_EQ(site->kernelCanonicalName, "test.M.saxpy");
    // KernelStream is the first positional launch() arg.
    EXPECT_NE(site->stream, nullptr);
    // 1-D grid and block, each one element.
    EXPECT_EQ(site->grid.size(), 1u);
    EXPECT_EQ(site->block.size(), 1u);
    // Four kernel arguments in the trailing (args).
    EXPECT_EQ(site->kernelArgs.size(), 4u);
}

// Regression (compiler-xpu review, finding #1): the body walk must descend
// into loop/if/try bodies, not just statement-level nodes. A grid-stride
// kernel reads its builtins inside the for-init and for-update — both in the
// ForStatement's private fields — and those must still be captured.
TEST(XpuMirLaunchSiteTests, bodyOpsCapturedInsideLoop) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Workgroup;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void grid(KernelBuffer<float32> y, uint32 n) {\n"
        "        for (uint32 i = KernelThread.globalIdX(); i < n;\n"
        "             i += Workgroup.dimX()) {\n"
        "            y[i] = 1.0f;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    auto k = findKernel(mir, ".grid");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::ThreadId, Axis::X), 1u);
    EXPECT_EQ(countOps(k->bodyOps, OpKind::WorkgroupDim, Axis::X), 1u);
}

// Regression (finding #1): a launch site inside a loop must be recorded — the
// AMDGPU flat-work-group-size bound is derived from launchSites, so a missed
// larger-block launch inside a loop would under-bound the kernel.
TEST(XpuMirLaunchSiteTests, launchSiteRecognizedInsideLoop) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> y, uint32 n) { }\n"
        "    public static void run(KernelBuffer<float32> y, uint32 n,\n"
        "                           int32 steps) {\n"
        "        for (int32 s = 0; s < steps; s += 1) {\n"
        "            k.launch(KernelStream.current(),\n"
        "                     grid: [n], block: [256])(y, n);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    ASSERT_NE(mir, nullptr);
    ASSERT_EQ(mir->launchSites.size(), 1u);
    EXPECT_EQ(mir->launchSites[0]->kernelCanonicalName, "test.M.k");
}

// No launch call → no launch sites.
TEST(XpuMirLaunchSiteTests, noLaunchSitesWhenAbsent) {
    auto src =
        "package test;\n"
        "public class M {\n"
        "    @Kernel public static void k() { }\n"
        "    public static void run() { int32 a = 1; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto mir = XpuMirBuilder::buildForModule(module);
    EXPECT_TRUE(mir->launchSites.empty());
}
