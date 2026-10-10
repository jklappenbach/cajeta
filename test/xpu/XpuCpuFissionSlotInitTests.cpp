//
// CajetaXPU CPU backend: barrier fission's workgroup-uniform slots start at
// zero (xpu-kernel-adaptor 4.2.1.12).
//
// A uniform local lives in one stack slot for the whole block, written by the
// work-items a region admits. When NO work-item reaches the region that
// initializes it -- every work-item already left through a per-work-item
// `return`, the last-block check of a pack tail -- a scaffold loop's header
// still runs once for the block and compares a slot nobody wrote. After
// mem2reg that is an `undef` phi feeding the loop's exit test, so the trip
// count is whatever the previous kernel left in a register or on the stack:
// iq3xxsQ8IdGateUpGluKernel spun every cpu worker for twenty minutes, and only
// after other tests had run. Zeroed at the true entry, the empty case runs
// eight trips that admit nobody and ends. GPU-free; the check is on the IR,
// because the wrong behaviour depends on garbage and a run proves nothing.
//
#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>

using cajeta::Compiler;
using cajeta::EmitMode;
using cajeta::XpuBackend;

namespace {

namespace fs = std::filesystem;

std::string compileToIr(const char* source, const std::string& entry) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xpu_slotinit_" + std::to_string(rng()));
    auto srcDir = base / "src" / "test";
    auto build = base / "build";
    fs::create_directories(srcDir);
    fs::create_directories(build);
    std::ofstream(srcDir / "M.cajeta") << source;

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Cpu);
    compiler.compile(entry, (base / "src").string(), build.string());

    std::string ir;
    for (auto& e : fs::recursive_directory_iterator(build))
        if (e.is_regular_file() && e.path().extension() == ".ll"
            && e.path().filename() != "cajeta.runtime.__stdlib__.ll") {
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf(); ir = ss.str();
        }
    fs::remove_all(base);
    return ir;
}

// The body of `define ... @<name>(...) { ... }`, or "" when absent.
std::string functionBody(const std::string& ir, const std::string& name) {
    size_t at = ir.find("@" + name + "(");
    if (at == std::string::npos) at = ir.find("@\"" + name + "\"(");
    if (at == std::string::npos) return "";
    size_t def = ir.rfind("define", at);
    size_t end = ir.find("\n}\n", at);
    if (def == std::string::npos || end == std::string::npos) return "";
    return ir.substr(def, end - def);
}

// The pack tail's shape, reduced: every work-item but one block-quarter
// leaves at a check read from Shared, then a guarded uniform loop whose
// counter is set inside the guard, and a wave op so the kernel takes the
// scaffold path.
const char* kTailSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Barrier;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void tail(KernelBuffer<float32> out, KernelBuffer<int32> who) {
        Shared<int32> tk = shared int32[1];
        uint32 tid = KernelThread.x();
        if (tid == 0) { tk[0] = who[0]; }
        Barrier.workgroup();
        if ((uint32) tk[0] != tid / 64) { return; }
        float32 m = 0.0f;
        if (tid < 32) {
            int32 k = 0;
            while (k < 8) {
                m = m + (float32) k;
                k = k + 1;
            }
        }
        float32 r = Wave.reduceMaxF32(m);
        if (tid < 32) { out[tid] = r; }
    }
    public static int32 run() { return 0; }
}
)CJ";

} // namespace

TEST(XpuCpuFissionSlotInit, aUniformSlotNoWorkItemWroteIsZeroNotUndef) {
    const std::string ir = compileToIr(kTailSrc, "test.M.run");
    const std::string body = functionBody(ir, "__cajeta_xpu_cpu_block.test.M.tail");
    ASSERT_FALSE(body.empty()) << "no cpu block function for M.tail in the IR";
    // A kernel local's slot is `<name>.slot` after fission; Shared memory is
    // `<kernel>_<name>.blk` and stays undefined on every backend.
    std::regex undefSlot(R"(%[A-Za-z_][\w.]*\.slot[\w.]* = phi [^\n]*\[ undef,)");
    std::smatch m;
    EXPECT_FALSE(std::regex_search(body, m, undefSlot))
        << "a uniform slot reaches a phi as undef: " << m.str(0);
}
