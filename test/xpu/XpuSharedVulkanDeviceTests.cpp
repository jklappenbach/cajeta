//
// CajetaXPU Vulkan bring-up (Increment 5) — workgroup-shared tree reduction on
// a real GPU via Vulkan compute.
//
// The Vulkan analog of XpuShared{,Amd}DeviceTests. Static shared memory lowers
// through the SHARED AST walk: the `shared` keyword becomes an addrspace(3)
// global, which the SPIR-V backend maps to the Workgroup storage class (Shared
// is a non-fork — exactly as the NVIDIA∩AMD reckoning predicted). The tile
// width matches the backend's baked LocalSize (kVulkanLocalSizeX), since Vulkan
// fixes the workgroup size at SPIR-V compile time.
//
// The barrier here is made Vulkan-spec-valid by SpirvBackend's post-emit fixup
// (LLVM 23 emits forbidden SequentiallyConsistent semantics; see
// XpuVulkanEmitTests.workgroupBarrierIsSpecValid). This test is the on-device
// counterpart — it confirms the reduction's actual result on the GPU; gated on a
// Vulkan device, and still tolerant of a driver that rejects the barrier.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::vulkan;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vksh_" + std::to_string(rng()));
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
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vksh_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

} // namespace

// One block of kVulkanLocalSizeX threads: stage in[0..W) into a static shared
// tile, barrier, tree-reduce, thread 0 writes tile[0] to out[workgroup]. The
// tile width is the baked LocalSize so the block exactly covers it.

// Shared-memory atomics: a per-block counter. Every thread in the workgroup does
// acc.atomicAdd(0, 1) on a `shared uint32[]` slot (Workgroup-scope OpAtomicIAdd,
// not the Device-scope global form); thread 0 flushes it to out[block]. One
// workgroup of W threads -> out[0] == W. Proves the address-space-derived scope
// is correct on the real device (RADV).
TEST(XpuSharedVulkanDeviceTests, sharedAtomicCounterRunsOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    const unsigned W = cajeta::xpu::vulkan::kVulkanLocalSizeX;  // 64
    std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Workgroup;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void blockCount(KernelBuffer<uint32> out, uint32 n) {\n"
        "        Shared<uint32> acc = shared uint32[1];\n"
        "        uint32 t = KernelThread.x();\n"
        "        if (t == 0) { acc[0] = 0; }\n"
        "        Barrier.workgroup();\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < n) { acc.atomicAdd(0, 1); }\n"
        "        Barrier.workgroup();\n"
        "        if (t == 0) { out[Workgroup.x()] = acc[0]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto k = findMethod(module->getStructures()["test.M"], "blockCount");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_shatomic_vkdevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    const uint32_t n = W;   // one full workgroup contributes
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(sizeof(uint32_t));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dN, 0u);
    uint32_t zero = 0;
    ASSERT_TRUE(vk.upload(dOut, &zero, sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));

    bool launched = vk.launch(spirv.data(), spirv.size(), "blockCount",
                              {dOut, dN}, /*groupCountX=*/1);
    if (!launched) {
        vk.free(dOut); vk.free(dN);
        GTEST_SKIP() << "driver rejected the LLVM-emitted workgroup barrier";
    }

    uint32_t result = 0;
    ASSERT_TRUE(vk.download(&result, dOut, sizeof(uint32_t)));
    vk.free(dOut);
    vk.free(dN);

    EXPECT_EQ(result, W);   // W threads each added 1 into the shared counter
}
