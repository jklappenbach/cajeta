//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 2) — kernel-body lowering
// through the SHARED AST walk with the AMDGPU LoweringTarget.
//
// This is the seam payoff: the SAME ~885-line AST walk that lowers NVPTX
// kernels lowers AMDGPU ones, with only LoweringTarget forking. The SAXPY +
// strided-loop sources here are byte-identical to the NVPTX emit tests; only
// the backend differs. Two assertion layers:
//   - IR structure (deterministic): the measured seam decisions — alloca in
//     addrspace(5), buffers in addrspace(1), amdgcn coordinate intrinsics,
//     the dispatch-packet block-dim read, amdgpu_kernel calling convention.
//   - ISA smoke: end-to-end AMDGPU codegen produces real gfx1151 ISA.
//
// GPU-free (ISA is text). On-device verification is XpuSaxpyAmdDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_amdgpu_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_amdgpu_arch_" + std::to_string(rng()));
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

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

} // namespace

// SAXPY lowers through the shared walk + AMDGPU target: the IR carries every
// measured seam decision, and the AMDGPU backend emits real gfx1151 ISA.
TEST(XpuAmdgpuLoopEmitTests, lowersSaxpyToIsaAndIr) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            y[i] = a * x[i] + y[i];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto klass = module->getStructures()["test.M"];
    ASSERT_NE(klass, nullptr);
    auto saxpy = findMethod(klass, "saxpy");
    ASSERT_NE(saxpy, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_amdgpu", deviceCtx);
    configureDeviceModule(deviceModule, *tm);

    llvm::Function* fn = lowerKernel(saxpy, deviceModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arg_size(), 4u);
    // Seam: AMDGPU kernels are marked by calling convention, no metadata.
    EXPECT_EQ(fn->getCallingConv(), llvm::CallingConv::AMDGPU_KERNEL);

    std::string ir = printModule(deviceModule);
    // Seam: mutable scalar slots allocate in the private address space (5).
    EXPECT_NE(ir.find("addrspace(5)"), std::string::npos) << ir;
    // Buffer params are device global memory (addrspace(1)).
    EXPECT_NE(ir.find("ptr addrspace(1)"), std::string::npos) << ir;
    // Coordinate reads use the amdgcn intrinsics; global id needs workitem +
    // workgroup id AND the dispatch-packet block-dim read.
    EXPECT_NE(ir.find("llvm.amdgcn.workitem.id.x"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.workgroup.id.x"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.dispatch.ptr"), std::string::npos) << ir;

    std::string isa = emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel saxpy"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_load"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;
}

// A strided-sum loop kernel (identical source to the NVPTX loop test) lowers
// to AMDGPU ISA with a loop backedge and global load/store traffic.
TEST(XpuAmdgpuLoopEmitTests, lowersStridedSumLoop) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void strideSum(Buffer<int32> out, Buffer<int32> in,\n"
        "                                  uint32 n, uint32 stride) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        int32 acc = 0;\n"
        "        for (uint32 j = i; j < n; j += stride) {\n"
        "            acc += in[j];\n"
        "        }\n"
        "        out[i] = acc;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto strideSum = findMethod(module->getStructures()["test.M"], "strideSum");
    ASSERT_NE(strideSum, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_stridesum_amdgpu", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    llvm::Function* fn = lowerKernel(strideSum, deviceModule);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->getCallingConv(), llvm::CallingConv::AMDGPU_KERNEL);

    std::string isa = emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find(".amdhsa_kernel strideSum"), std::string::npos) << isa;
    // A loop: at least one conditional/uniform branch (the backedge / guard).
    EXPECT_TRUE(isa.find("s_cbranch") != std::string::npos ||
                isa.find("s_branch") != std::string::npos) << isa;
    EXPECT_NE(isa.find("global_load"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_endpgm"), std::string::npos) << isa;
}
