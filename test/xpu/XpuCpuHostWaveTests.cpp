// The cpu backend's reported wave is the wave its kernels were built at.
//
// Device.waveSize() on the cpu backend answers the width the registration
// recorded (__cajeta_xpu_set_cpu_wave), which a launcher uses to size its
// block: cajeta-llm's wave-per-row family launches `rowsPerBlock * waveSize`
// threads. A wave kernel's own width is read off its wrapper AFTER the wrapper
// is tuned (tune-cpu=generic, 6.4.12), and the recorded width was read off an
// untuned probe. On an AVX-512 part whose tuning prefers 256-bit vectors
// (Intel Ice Lake and its Xeon siblings, GitHub's ubuntu-latest among them)
// the two differ: the kernel vectorizes at 16 and the probe answers 8, so the
// launcher asks for a block of 8 and the launch is refused as the scalar twin
// (cajeta-cabra CI 37495815557, 2026-10-06, ten tests; before the twin
// refusal the same launch silently ran the wave ops at width 1). Reproduced
// here with the cost-model overrides, with no kernel executed.
#include "gtest/gtest.h"

#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <optional>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void wsum(KernelBuffer<float32> y, uint32 n) {
        float32 v = (float32) (KernelThread.x() % 32);
        float32 t = Wave.reduceSumF32(v);
        if (KernelThread.x() == 0) { y[0] = t; }
    }
    public static int32 reportedWave() { return Device.waveSize(); }
}
)CJ";

// The width `wsum` is built at, off its manifest.
std::optional<unsigned> builtWave() {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == "wsum") k = m;
    if (!k) return std::nullopt;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_hostwave_cpu", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1) return std::nullopt;
    return out[0].waveWidth;
}

// What Device.waveSize() answers once the same program's kernels registered.
std::optional<int> reportedWave() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return std::nullopt;
    auto fn = jit->lookup<int (*)()>("reportedWave");
    if (!fn) return std::nullopt;
    return fn();
}

struct CostModelOverride {
    CostModelOverride(const char* cpu, const char* features) {
        setenv("CAJETA_XPU_CPU_MCPU", cpu, 1);
        setenv("CAJETA_XPU_CPU_MATTR", features, 1);
    }
    ~CostModelOverride() {
        unsetenv("CAJETA_XPU_CPU_MCPU");
        unsetenv("CAJETA_XPU_CPU_MATTR");
    }
};

} // namespace

// On this host, as built: the two agree.
TEST(XpuCpuHostWave, theReportedWaveIsTheBuiltWaveOnThisHost) {
    auto built = builtWave();
    ASSERT_TRUE(built.has_value());
    auto reported = reportedWave();
    ASSERT_TRUE(reported.has_value());
    EXPECT_EQ((unsigned) *reported, *built);
}

// On an AVX-512 part tuned to prefer 256-bit vectors, cost-modelled here: the
// kernel is built at the width the tuned wrapper vectorizes at, and the
// reported wave must be that width, not the untuned probe's.
#if defined(__x86_64__) && !defined(_WIN32)
TEST(XpuCpuHostWave, theReportedWaveIsTheBuiltWaveOnAPrefer256Avx512Host) {
    CostModelOverride host("icelake-server", "+avx512f,+avx512bw,+avx512vl,+avx512dq");
    auto built = builtWave();
    ASSERT_TRUE(built.has_value());
    EXPECT_GE(*built, 2u) << "a wave kernel vectorizes on an AVX-512 host";
    auto reported = reportedWave();
    ASSERT_TRUE(reported.has_value());
    EXPECT_EQ((unsigned) *reported, *built)
        << "Device.waveSize() answers " << *reported << " while wsum is built at "
        << *built << ": a launcher sizing its block from the reported wave "
        "launches a block that is not a multiple of the kernel's wave";
}
#endif
