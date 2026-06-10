//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 5) — workgroup shared
// memory (LDS) on a real AMD GPU (gfx1151).
//
// The AMD analog of XpuSharedDeviceTests. The SAME reduction sources that run
// the addrspace(3) + barrier path on NVIDIA run here through the shared
// lowerer + AMDGPU LoweringTarget: static LDS lowers to an internal
// addrspace(3) global (group-segment-fixed-size), dynamic LDS to an external
// unsized addrspace(3) global sized at launch via hipModuleLaunchKernel's
// sharedMemBytes (= groupMemBytes). The barrier is amdgcn s_barrier wrapped in
// workgroup-scoped fences for LDS visibility.
//
// Skips cleanly when no ROCm/HIP device is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_amdshared_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_amdshared_arch_" + std::to_string(rng()));
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

std::vector<uint8_t> buildHsaco(Compiler& compiler, const char* src,
                                const std::string& kernelName) {
    auto module = compileForInspection(compiler, src, "test.M");
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_amdshared_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return assembleHsaco(deviceModule, *tm, "gfx1151");
}

} // namespace

TEST(XpuSharedAmdDeviceTests, sharedTreeReductionRunsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }

    // One block of 256 threads: stage in[0..256) into a static shared tile,
    // barrier, tree-reduce, thread 0 writes tile[0] to out[blockIdx].
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "import cajeta.gpu.core.Workgroup;\n"
        "import cajeta.gpu.core.Barrier;\n"
        "import cajeta.gpu.core.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void reduce(Buffer<int32> out, Buffer<int32> in, uint32 n) {\n"
        "        Shared<int32> tile = shared int32[256];\n"
        "        uint32 t = Thread.x();\n"
        "        uint32 g = Thread.globalIdX();\n"
        "        if (g < n) {\n"
        "            tile[t] = in[g];\n"
        "        } else {\n"
        "            tile[t] = 0;\n"
        "        }\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
        "            if (t < s) {\n"
        "                tile[t] += tile[t + s];\n"
        "            }\n"
        "            Barrier.workgroup();\n"
        "        }\n"
        "        if (t == 0) {\n"
        "            out[Workgroup.x()] = tile[0];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    std::vector<uint8_t> hsaco = buildHsaco(compiler, src, "reduce");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    // in[i] = i  ->  expected sum over the 256-wide block = 256*255/2.
    const uint32_t n = 256;
    std::vector<int32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = (int32_t) i;
    const int32_t expected = (int32_t) ((uint64_t) n * (n - 1) / 2);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "reduce");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dOut = hip.alloc(sizeof(int32_t));
    HipDevicePtr dIn = hip.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dOut, nullptr);
    ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(int32_t)));

    void* params[] = { &dOut, &dIn, (void*) &n };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/256, params));
    ASSERT_TRUE(hip.synchronize());

    int32_t result = -1;
    ASSERT_TRUE(hip.memcpyDtoH(&result, dOut, sizeof(int32_t)));
    hip.free(dOut);
    hip.free(dIn);

    EXPECT_EQ(result, expected);
}

// Same reduction, but the tile is DYNAMIC LDS: its size is a runtime kernel
// arg, so the device global is external/unsized and the byte count is supplied
// at launch (hipModuleLaunchKernel sharedMemBytes / groupMemBytes).
TEST(XpuSharedAmdDeviceTests, dynamicSharedReductionRunsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }

    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "import cajeta.gpu.core.Workgroup;\n"
        "import cajeta.gpu.core.Barrier;\n"
        "import cajeta.gpu.core.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void dynreduce(Buffer<int32> out, Buffer<int32> in,\n"
        "                                 uint32 n, uint32 tileLen) {\n"
        "        Shared<int32> tile = shared int32[tileLen];\n"
        "        uint32 t = Thread.x();\n"
        "        uint32 g = Thread.globalIdX();\n"
        "        if (g < n) {\n"
        "            tile[t] = in[g];\n"
        "        } else {\n"
        "            tile[t] = 0;\n"
        "        }\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
        "            if (t < s) {\n"
        "                tile[t] += tile[t + s];\n"
        "            }\n"
        "            Barrier.workgroup();\n"
        "        }\n"
        "        if (t == 0) {\n"
        "            out[Workgroup.x()] = tile[0];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    std::vector<uint8_t> hsaco = buildHsaco(compiler, src, "dynreduce");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 256;
    const uint32_t tileLen = 256;
    std::vector<int32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = (int32_t) i;
    const int32_t expected = (int32_t) ((uint64_t) n * (n - 1) / 2);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "dynreduce");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dOut = hip.alloc(sizeof(int32_t));
    HipDevicePtr dIn = hip.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dOut, nullptr);
    ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(int32_t)));

    // The launch supplies the dynamic LDS byte count: tileLen int32s.
    void* params[] = { &dOut, &dIn, (void*) &n, (void*) &tileLen };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/256, params,
                           /*sharedMemBytes=*/tileLen * sizeof(int32_t)));
    ASSERT_TRUE(hip.synchronize());

    int32_t result = -1;
    ASSERT_TRUE(hip.memcpyDtoH(&result, dOut, sizeof(int32_t)));
    hip.free(dOut);
    hip.free(dIn);

    EXPECT_EQ(result, expected);
}

// Shared-memory atomics on AMD (gfx1151): a per-block counter. Every thread does
// acc.atomicAdd(0, 1) on a `shared uint32[]` slot (an LDS atomic — the addrspace(3)
// pointer selects ds_add_u32, not a global atomic); thread 0 flushes it to
// out[block]. One block of 256 threads -> out[0] == 256.
TEST(XpuSharedAmdDeviceTests, sharedAtomicCounterRunsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "import cajeta.gpu.core.Workgroup;\n"
        "import cajeta.gpu.core.Barrier;\n"
        "import cajeta.gpu.core.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void blockCount(Buffer<uint32> out, uint32 n) {\n"
        "        Shared<uint32> acc = shared uint32[1];\n"
        "        uint32 t = Thread.x();\n"
        "        if (t == 0) { acc[0] = 0; }\n"
        "        Barrier.workgroup();\n"
        "        uint32 g = Thread.globalIdX();\n"
        "        if (g < n) { acc.atomicAdd(0, 1); }\n"
        "        Barrier.workgroup();\n"
        "        if (t == 0) { out[Workgroup.x()] = acc[0]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    std::vector<uint8_t> hsaco = buildHsaco(compiler, src, "blockCount");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 256;   // one block of 256 threads all contribute
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "blockCount");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dOut = hip.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    uint32_t zero = 0;
    ASSERT_TRUE(hip.memcpyHtoD(dOut, &zero, sizeof(uint32_t)));

    void* params[] = { &dOut, (void*) &n };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/256, params));
    ASSERT_TRUE(hip.synchronize());

    uint32_t result = 0;
    ASSERT_TRUE(hip.memcpyDtoH(&result, dOut, sizeof(uint32_t)));
    hip.free(dOut);

    EXPECT_EQ(result, 256u);
}
