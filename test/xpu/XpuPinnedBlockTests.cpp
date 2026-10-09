// workgroup-reduce Unit 8: a kernel whose every launch site passes the same constant block
// is built for that block, its manifest records it, and the runtime refuses any other.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/core/LaunchBlocks.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <cstring>
#include <string>

extern "C" void __cajeta_xpu_register_kernel_manifest(const char* kernelName, int32_t backend,
                                                      const char* arch, const void* json,
                                                      uint64_t len);
extern "C" void __cajeta_xpu_launch_v3(const char* kernelName, int32_t gridX, int32_t gridY,
                                       int32_t gridZ, int32_t blockX, int32_t blockY,
                                       int32_t blockZ, uint32_t sharedBytes, void* argv,
                                       int64_t streamHandle, int32_t deviceId,
                                       int32_t specCount, const int32_t* specValues);
extern "C" int32_t __cajeta_xpu_active_backend_id(void);
extern "C" int32_t __cajeta_xpu_last_launch_failed(void);
extern "C" int32_t __cajeta_xpu_last_launch_reason(void);

namespace {

namespace xpu = cajeta::xpu;
using xpu::probe::compileForInspection;
using xpu::probe::findMethod;
using Block = std::array<unsigned, 3>;

const char* kSrc =
    "package test;\n"
    "import cajeta.xpu.GroupOp;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Workgroup;\n"
    "public class P {\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void pinned(KernelBuffer<float32> out, KernelBuffer<int32> dims) {\n"
    "        float32 s = Workgroup.reduce(GroupOp.Add, 1.0f);\n"
    "        if (KernelThread.x() == 0) {\n"
    "            out[KernelThread.globalIdX()] = s;\n"
    "            dims[Workgroup.x()] = (int32) Workgroup.dimX();\n"
    "        }\n"
    "    }\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void split(KernelBuffer<float32> out, KernelBuffer<int32> dims) {\n"
    "        float32 s = Workgroup.reduce(GroupOp.Add, 1.0f);\n"
    "        if (KernelThread.x() == 0) {\n"
    "            out[KernelThread.globalIdX()] = s;\n"
    "            dims[Workgroup.x()] = (int32) Workgroup.dimX();\n"
    "        }\n"
    "    }\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void loose(KernelBuffer<float32> out, KernelBuffer<int32> dims) {\n"
    "        float32 s = Workgroup.reduce(GroupOp.Add, 1.0f);\n"
    "        if (KernelThread.x() == 0) { out[Workgroup.x()] = s; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void unlaunched(KernelBuffer<float32> out) {\n"
    "        out[KernelThread.x()] = (float32) Workgroup.dimX();\n"
    "    }\n"
    "    public static void go(KernelStream s, KernelBuffer<float32> out,\n"
    "            KernelBuffer<int32> dims, uint32 n) {\n"
    "        pinned.launch(s, grid: [1], block: [256])(out, dims);\n"
    "        pinned.launch(s, grid: [2], block: [256])(out, dims);\n"
    "        split.launch(s, grid: [1], block: [256])(out, dims);\n"
    "        split.launch(s, grid: [1], block: [128])(out, dims);\n"
    "        loose.launch(s, grid: [1], block: [n])(out, dims);\n"
    "    }\n"
    "}\n";

struct Scanned {
    cajeta::Compiler compiler;
    cajeta::CajetaModulePtr module;
    xpu::LaunchBlocks blocks;
    cajeta::MethodPtr kernel(const std::string& name) {
        return findMethod(module->getStructures()["test.P"], name);
    }
};

std::unique_ptr<Scanned> scan() {
    auto s = std::make_unique<Scanned>();
    s->module = compileForInspection(s->compiler, kSrc, "test.P");
    s->blocks = xpu::scanLaunchBlocks({s->module});
    return s;
}

std::string lowered(const cajeta::MethodPtr& k, const std::string& be) {
    llvm::LLVMContext ctx;
    llvm::Module dev("pinned_probe", ctx);
    if (be == "amdgpu") {
        auto tm = xpu::amd::createAmdgpuTargetMachine("gfx1151");
        if (!tm) return "";
        xpu::amd::configureDeviceModule(dev, *tm);
        xpu::amd::lowerKernel(k, dev);
    } else if (be == "nvptx") {
        auto tm = xpu::nvidia::createNvptxTargetMachine("sm_89");
        if (!tm) return "";
        xpu::nvidia::configureDeviceModule(dev, *tm);
        xpu::nvidia::lowerKernel(k, dev);
    } else {
        auto tm = xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
        if (!tm) return "";
        xpu::vulkan::configureDeviceModule(dev, *tm);
        xpu::vulkan::lowerKernel(k, dev);
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    dev.print(os, nullptr);
    return os.str();
}

// 8.1.1
TEST(XpuPinnedBlockTests, everySiteAgreeingPinsTheBlock) {
    auto s = scan();
    auto it = s->blocks.pinned.find("test.P.pinned");
    ASSERT_NE(it, s->blocks.pinned.end());
    EXPECT_EQ(it->second, (Block{256, 1, 1}));
    ASSERT_TRUE(s->kernel("pinned")->pinnedLaunchBlock().has_value());
    EXPECT_EQ(*s->kernel("pinned")->pinnedLaunchBlock(), (Block{256, 1, 1}));
}

TEST(XpuPinnedBlockTests, disagreeingNonConstantOrUnlaunchedSitesPinNothing) {
    auto s = scan();
    for (const char* k : {"split", "loose", "unlaunched"}) {
        EXPECT_EQ(s->blocks.pinned.count(std::string("test.P.") + k), 0u) << k;
        EXPECT_FALSE(s->kernel(k)->pinnedLaunchBlock().has_value()) << k;
    }
    EXPECT_EQ(s->blocks.maxThreads["split"], 256u);
    EXPECT_EQ(s->blocks.maxThreads.count("loose"), 0u);
    EXPECT_EQ(s->blocks.unboundedSites["loose"], 1u);
}

TEST(XpuPinnedBlockTests, aRescanClearsAPinTheSitesNoLongerSupport) {
    auto s = scan();
    s->kernel("split")->setPinnedLaunchBlock(Block{64, 1, 1});
    xpu::scanLaunchBlocks({s->module});
    EXPECT_FALSE(s->kernel("split")->pinnedLaunchBlock().has_value());
}

// 8.1.2
TEST(XpuPinnedBlockTests, aPinnedKernelReadsNoWorkgroupSizeOnAmdgpu) {
    auto s = scan();
    std::string pinned = lowered(s->kernel("pinned"), "amdgpu");
    if (pinned.empty()) GTEST_SKIP() << "no amdgpu target";
    EXPECT_EQ(pinned.find("llvm.amdgcn.dispatch.ptr"), std::string::npos) << pinned;
    std::string split = lowered(s->kernel("split"), "amdgpu");
    EXPECT_NE(split.find("llvm.amdgcn.dispatch.ptr"), std::string::npos);
}

TEST(XpuPinnedBlockTests, aPinnedKernelReadsNoWorkgroupSizeOnNvptx) {
    auto s = scan();
    std::string pinned = lowered(s->kernel("pinned"), "nvptx");
    if (pinned.empty()) GTEST_SKIP() << "no nvptx target";
    EXPECT_EQ(pinned.find("llvm.nvvm.read.ptx.sreg.ntid"), std::string::npos) << pinned;
    std::string split = lowered(s->kernel("split"), "nvptx");
    EXPECT_NE(split.find("llvm.nvvm.read.ptx.sreg.ntid"), std::string::npos);
}

TEST(XpuPinnedBlockTests, aPinnedKernelReadsNoWorkgroupSizeOnVulkan) {
    auto s = scan();
    std::string pinned = lowered(s->kernel("pinned"), "spirv");
    if (pinned.empty()) GTEST_SKIP() << "no spirv target";
    EXPECT_EQ(pinned.find(xpu::vulkan::kWorkgroupDimWitness), std::string::npos) << pinned;
    std::string split = lowered(s->kernel("split"), "spirv");
    EXPECT_NE(split.find(xpu::vulkan::kWorkgroupDimWitness), std::string::npos);
}

// 8.1.3
TEST(XpuPinnedBlockTests, theManifestRecordsThePinnedBlock) {
    auto s = scan();
    std::vector<cajeta::MethodPtr> kernels = {s->kernel("pinned"), s->kernel("split")};
    llvm::LLVMContext ctx;
    llvm::Module host("pinned_host", ctx);
    std::vector<xpu::KernelManifest> manifests;
    xpu::emitKernelRegistration(xpu::Backend::Cpu, kernels, host, "", {}, &manifests);
    ASSERT_EQ(manifests.size(), 2u);
    for (const auto& m : manifests) {
        std::string json = xpu::toJson(m);
        if (m.kernel == "test.P.pinned") {
            ASSERT_TRUE(m.block.has_value());
            EXPECT_EQ(*m.block, (Block{256, 1, 1}));
            xpu::KernelManifest back;
            ASSERT_TRUE(xpu::fromJson(json, back)) << json;
            EXPECT_EQ(back.block, m.block);
        } else {
            EXPECT_FALSE(m.block.has_value());
            EXPECT_EQ(json.find("\"block\""), std::string::npos) << json;
        }
    }
}

// 8.1.4
const char* kManifest =
    "{\"schemaVersion\":1,\"identity\":{\"kernel\":\"test.Q.wgPinnedProbe\"},"
    "\"footprint\":{\"waveWidth\":32,\"block\":[256,1,1]}}";

TEST(XpuPinnedBlockTests, theRuntimeRefusesABlockOtherThanThePinnedOne) {
    const char* name = "wgPinnedProbe";
    __cajeta_xpu_register_kernel_manifest(name, __cajeta_xpu_active_backend_id(), "probe",
                                          kManifest, std::strlen(kManifest));
    testing::internal::CaptureStderr();
    __cajeta_xpu_launch_v3(name, 1, 1, 1, 128, 1, 1, 0, nullptr, 0, -1, 0, nullptr);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(__cajeta_xpu_last_launch_failed(), 1);
    EXPECT_EQ(__cajeta_xpu_last_launch_reason(), 4);
    EXPECT_NE(err.find("wgPinnedProbe"), std::string::npos) << err;
    EXPECT_NE(err.find("[256, 1, 1]"), std::string::npos) << err;
    EXPECT_NE(err.find("[128, 1, 1]"), std::string::npos) << err;
}

TEST(XpuPinnedBlockTests, theRuntimeDoesNotRefuseThePinnedBlock) {
    const char* name = "wgPinnedProbe";
    __cajeta_xpu_register_kernel_manifest(name, __cajeta_xpu_active_backend_id(), "probe",
                                          kManifest, std::strlen(kManifest));
    testing::internal::CaptureStderr();
    __cajeta_xpu_launch_v3(name, 1, 1, 1, 256, 1, 1, 0, nullptr, 0, -1, 0, nullptr);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(__cajeta_xpu_last_launch_failed() == 1 && __cajeta_xpu_last_launch_reason() == 4);
    EXPECT_EQ(err.find("was built for block"), std::string::npos) << err;
}

TEST(XpuPinnedBlockTests, checkLaunchNamesTheBlockRefusal) {
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {xpu::Backend::Cpu};
    auto jit = cajeta_test::CajetaJit::compile(
        "package test;\n"
        "import cajeta.xpu.Device;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.XpuLaunchException;\n"
        "public class B {\n"
        "    @Kernel\n"
        "    public static void wgPinnedJit(KernelBuffer<float32> out) {\n"
        "        out[KernelThread.x()] = 1.0f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        KernelBuffer<float32> out #= heap KernelBuffer<float32>(256L);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        int32 r = 0;\n"
        "        try {\n"
        "            wgPinnedJit.launch(s, grid: [1], block: [256])(out);\n"
        "        } catch (XpuLaunchException e) {\n"
        "            r = 1;\n"
        "            if (e.message.contains(\"block\")) { r = 4; }\n"
        "        }\n"
        "        s.sync();\n"
        "        return r;\n"
        "    }\n"
        "}\n",
        "test.B", o);
    auto run = jit->lookup<int32_t (*)()>("run");
    testing::internal::CaptureStderr();
    int32_t own = run();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(own, 0);
    const char* forced =
        "{\"schemaVersion\":1,\"identity\":{\"kernel\":\"test.B.wgPinnedJit\"},"
        "\"footprint\":{\"block\":[64,1,1]}}";
    using RegFn = void (*)(const char*, int32_t, const char*, const void*, uint64_t);
    using BeFn = int32_t (*)();
    auto reg = reinterpret_cast<RegFn>(jit->lookupRawSymbol("__cajeta_xpu_register_kernel_manifest"));
    auto be = reinterpret_cast<BeFn>(jit->lookupRawSymbol("__cajeta_xpu_active_backend_id"));
    ASSERT_NE(reg, nullptr);
    ASSERT_NE(be, nullptr);
    reg("wgPinnedJit", be(), "", forced, std::strlen(forced));
    testing::internal::CaptureStderr();
    int32_t r = run();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 4);
}

}  // namespace
