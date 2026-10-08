// workgroup-reduce Unit 4: misuse of Workgroup.reduce is refused by name on every backend,
// and each refusal has a twin that lowers.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <map>
#include <string>

namespace {

namespace xpu = cajeta::xpu;
using xpu::probe::compileForInspection;
using xpu::probe::findMethod;

const char* kBackends[] = {"cpu", "nvptx", "amdgpu", "spirv"};

std::string wrap(const std::string& kernels) {
    return "package test;\n"
           "import cajeta.xpu.GroupOp;\n"
           "import cajeta.xpu.KernelBuffer;\n"
           "import cajeta.xpu.KernelThread;\n"
           "import cajeta.xpu.Workgroup;\n"
           "public class R {\n" + kernels + "}\n";
}

// The refusal each backend's lowering gives `kernel`, or "" when it lowers.
std::map<std::string, std::string> lowerOnEvery(const std::string& src, const std::string& kernel) {
    std::map<std::string, std::string> why;
    cajeta::Compiler compiler;
    cajeta::CajetaModulePtr module;
    try {
        module = compileForInspection(compiler, src, "test.R");
    } catch (cajeta::Exception& e) {
        for (const char* be : kBackends) why[be] = e.getMessage();
        return why;
    }
    auto k = findMethod(module->getStructures()["test.R"], kernel);
    if (!k) {
        for (const char* be : kBackends) why[be] = "no kernel " + kernel;
        return why;
    }
    for (const std::string be : kBackends) {
        llvm::LLVMContext ctx;
        llvm::Module dev("refusal_probe", ctx);
        try {
            if (be == "cpu") {
                xpu::cpu::lowerKernel(k, dev);
            } else if (be == "nvptx") {
                auto tm = xpu::nvidia::createNvptxTargetMachine("sm_89");
                if (!tm) continue;
                xpu::nvidia::configureDeviceModule(dev, *tm);
                xpu::nvidia::lowerKernel(k, dev);
            } else if (be == "amdgpu") {
                auto tm = xpu::amd::createAmdgpuTargetMachine("gfx1151");
                if (!tm) continue;
                xpu::amd::configureDeviceModule(dev, *tm);
                xpu::amd::lowerKernel(k, dev);
            } else {
                auto tm = xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
                if (!tm) continue;
                xpu::vulkan::configureDeviceModule(dev, *tm);
                xpu::vulkan::lowerKernel(k, dev);
            }
            why[be] = "";
        } catch (cajeta::Exception& e) {
            why[be] = e.getMessage();
        } catch (const std::exception& e) {
            why[be] = e.what();
        }
    }
    return why;
}

void refusedEverywhere(const std::string& kernels, const std::string& kernel,
                       const std::string& cause) {
    auto why = lowerOnEvery(wrap(kernels), kernel);
    EXPECT_FALSE(why.empty());
    for (auto& [be, w] : why) {
        EXPECT_NE(w.find("Workgroup.reduce"), std::string::npos) << be << ": " << w;
        EXPECT_NE(w.find(cause), std::string::npos) << be << ": " << w;
    }
}

void lowersEverywhere(const std::string& kernels, const std::string& kernel) {
    auto why = lowerOnEvery(wrap(kernels), kernel);
    EXPECT_FALSE(why.empty());
    for (auto& [be, w] : why) EXPECT_EQ(w, "") << be;
}

const char* kDivergent = "work-item-divergent";

}  // namespace

// 4.2.1: a call under a branch on the lane's position.
TEST(XpuWorkgroupReduceRefusal, aCallUnderALaneGuardIsRefused) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) {
            out[g] = Workgroup.reduce(GroupOp.Add, in[g]);
        }
    }
)CJ", "k", kDivergent);
}

TEST(XpuWorkgroupReduceRefusal, aLaneGuardOverTheValueOnlyLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        float32 x = 0.0f;
        if (g < n) {
            x = in[g];
        }
        out[g] = Workgroup.reduce(GroupOp.Add, x);
    }
)CJ", "k");
}

// 4.2.1: a lane that returns before the call never contributes.
TEST(XpuWorkgroupReduceRefusal, anEarlyReturnBeforeTheCallIsRefused) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g >= n) {
            return;
        }
        out[g] = Workgroup.reduce(GroupOp.Max, in[g]);
    }
)CJ", "k", kDivergent);
}

TEST(XpuWorkgroupReduceRefusal, aGuardOnAKernelParameterLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (n > 0) {
            out[g] = Workgroup.reduce(GroupOp.Max, in[g]);
        }
    }
)CJ", "k");
}

// 4.2.1: a loop whose trip count is the lane's own.
TEST(XpuWorkgroupReduceRefusal, aCallInALaneCountedLoopIsRefused) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 t = KernelThread.x();
        int32 acc = 0;
        uint32 j = 0;
        while (j < t) {
            acc = acc + Workgroup.reduce(GroupOp.Add, in[j]);
            j = j + 1;
        }
        out[t] = acc;
    }
)CJ", "k", kDivergent);
}

TEST(XpuWorkgroupReduceRefusal, aCallInAWorkgroupCountedLoopLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 t = KernelThread.x();
        int32 acc = 0;
        uint32 j = 0;
        while (j < Workgroup.x() + 2) {
            acc = acc + Workgroup.reduce(GroupOp.Add, in[t + j]);
            j = j + 1;
        }
        out[t] = acc;
    }
)CJ", "k");
}

// 4.2.1: a flag set under a lane guard carries the divergence to a later branch.
TEST(XpuWorkgroupReduceRefusal, aFlagSetUnderALaneGuardIsRefused) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 t = KernelThread.x();
        boolean on = false;
        if (t < 7) {
            on = true;
        }
        if (on) {
            out[t] = Workgroup.reduce(GroupOp.Min, in[t]);
        }
    }
)CJ", "k", kDivergent);
}

// A reduce's result is the same on every lane, so a branch on it is uniform.
TEST(XpuWorkgroupReduceRefusal, aBranchOnAReduceResultLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 t = KernelThread.x();
        float32 s = Workgroup.reduce(GroupOp.Add, in[t]);
        if (s > 0.0f) {
            out[t] = Workgroup.reduce(GroupOp.Max, in[t]);
        }
    }
)CJ", "k");
}

// 4.2.2: the operator is a compile-time GroupOp.
TEST(XpuWorkgroupReduceRefusal, anOperatorHeldInALocalIsRefused) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 t = KernelThread.x();
        GroupOp op = GroupOp.Add;
        out[t] = Workgroup.reduce(op, in[t]);
    }
)CJ", "k", "compile-time GroupOp");
}

TEST(XpuWorkgroupReduceRefusal, aLiteralOperatorLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 t = KernelThread.x();
        out[t] = Workgroup.reduce(GroupOp.Min, in[t]);
    }
)CJ", "k");
}

// 4.2.3: the value is a float32 or an int32, and a refusal names the type it was given.
TEST(XpuWorkgroupReduceRefusal, aUint32ValueIsRefusedByName) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 t = KernelThread.x();
        out[t] = (uint32) Workgroup.reduce(GroupOp.Max, in[t]);
    }
)CJ", "k", "uint32");
}

TEST(XpuWorkgroupReduceRefusal, aFloat64ValueIsRefusedByName) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<float64> out, KernelBuffer<float64> in) {
        uint32 t = KernelThread.x();
        out[t] = (float64) Workgroup.reduce(GroupOp.Add, in[t]);
    }
)CJ", "k", "float64");
}

TEST(XpuWorkgroupReduceRefusal, anInt64ValueIsRefusedByName) {
    refusedEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<int64> out, KernelBuffer<int64> in) {
        uint32 t = KernelThread.x();
        out[t] = (int64) Workgroup.reduce(GroupOp.Add, in[t]);
    }
)CJ", "k", "int64");
}

TEST(XpuWorkgroupReduceRefusal, anInt32ValueLowers) {
    lowersEverywhere(R"CJ(
    @Kernel
    public static void k(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 t = KernelThread.x();
        out[t] = Workgroup.reduce(GroupOp.Max, in[t]);
    }
)CJ", "k");
}

// 4.2.4: a host call is a workgroup of one lane, as Group.reduce and Barrier.workgroup() are.
TEST(XpuWorkgroupReduceRefusal, aHostCallActsAsAOneLaneWorkgroup) {
    auto jit = cajeta_test::CajetaJit::compile(wrap(R"CJ(
    public static float32 f() { return Workgroup.reduce(GroupOp.Add, 2.5f); }
    public static int32 i() { return Workgroup.reduce(GroupOp.Min, -7); }
)CJ"), "test.R");
    ASSERT_NE(jit, nullptr);
    auto f = jit->lookup<float (*)()>("f");
    auto i = jit->lookup<int32_t (*)()>("i");
    ASSERT_TRUE(f && i);
    EXPECT_EQ(f(), 2.5f);
    EXPECT_EQ(i(), -7);
}
