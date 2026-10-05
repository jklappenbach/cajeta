// A name in a kernel resolves to the binding the author named, or is refused
// (xpu-kernel-independence 4.2.1.2, spec §2.3).
//
// cajeta-llm 0f1ee39: a fused mat-vec's Q6_K body read `xr + 288` for its
// activation scale, and `xr` was declared only inside the Q4_K body's loop, a
// sibling block that never ran for a Q6_K row. The lowerer kept every local
// in one function-wide table, so the read found the stale slot and every
// block took a neighbouring block's scale for twelve days. A kernel follows
// the host's scopes: a local is visible from its declaration to the end of
// the block that declares it, and nowhere else.

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "KernelLoweringProbe.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string program(const std::string& kernelBody) {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void k(KernelBuffer<float32> y, uint32 which) {
        uint32 i = KernelThread.globalIdX();
)CJ" + kernelBody + R"CJ(
    }
    public static float32 run(uint32 which) {
        float32[] h = heap float32[8];
        for (uint32 j = 0; j < 8; j = j + 1) { h[j] = (float32) j; }
        KernelBuffer<float32> b = heap KernelBuffer<float32>(8);
        b.upload(h);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [8])(b, which);
        s.sync();
        b.download(h);
        return h[3];
    }
}
)CJ";
}

// 0f1ee39's shape: `xr` lives in the first branch and the second reads it.
const char* kSiblingRead =
    "        if (which == 0) { float32 xr = y[i] * 2.0f; y[i] = xr; }\n"
    "        else { y[i] = xr + 1.0f; }\n";

// The same, read after the block that declared it has closed.
const char* kReadAfterBlock =
    "        if (which == 0) { float32 t = y[i] * 2.0f; y[i] = t; }\n"
    "        y[i] = t;\n";

// A loop counter read past its loop.
const char* kCounterAfterLoop =
    "        for (uint32 kt = 0; kt < 2; kt = kt + 1) { y[i] = y[i] + 1.0f; }\n"
    "        y[i] = y[i] + (float32) kt;\n";

// The scopes a kernel may use: an outer local read inside nested blocks and
// a loop, one name declared in two sibling blocks with two types, and a loop
// counter declared by two loops in turn.
const char* kProperScopes =
    "        float32 base = y[i];\n"
    "        if (which == 0) {\n"
    "            float32 t = base * 2.0f;\n"
    "            for (uint32 kt = 0; kt < 3; kt = kt + 1) { t = t + base; }\n"
    "            y[i] = t;\n"
    "        } else {\n"
    "            int32 t = 5;\n"
    "            for (uint32 kt = 0; kt < 2; kt = kt + 1) { t = t + 1; }\n"
    "            y[i] = base + (float32) t;\n"
    "        }\n";

float runOnCpu(const std::string& src, unsigned which) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1000.0f;
    auto fn = jit->lookup<float (*)(unsigned)>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn(which) : -1000.0f;
}

} // namespace

// A name declared in a sibling or a closed block is refused, naming it.
TEST(XpuKernelScope, aNameFromAnotherBlockIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    for (auto [body, name] : {std::pair{kSiblingRead, "xr"}, std::pair{kReadAfterBlock, "t"},
                              std::pair{kCounterAfterLoop, "kt"}}) {
        Lowered l = lowerForNvptx(program(body), "k");
        EXPECT_FALSE(l.ok) << "lowered a read of `" << name << "` outside its block:\n" << body;
        EXPECT_NE(l.why.find(std::string("'") + name + "'"), std::string::npos) << l.why;
        EXPECT_NE(l.why.find("not in scope"), std::string::npos) << l.why;
    }
}

// The counterpart: the scopes a kernel legitimately uses lower, on nvptx
// and on cpu, where the answer is the host's. y[3] = 3: which 0 gives
// 3*2 + 3*3 = 15, which 1 gives 3 + 7 = 10.
TEST(XpuKernelScope, namesFromEnclosingBlocksAndSiblingRedeclarationsLower) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(program(kProperScopes), "k");
    EXPECT_TRUE(l.ok) << l.why;
    EXPECT_EQ(runOnCpu(program(kProperScopes), 0), 15.0f);
    EXPECT_EQ(runOnCpu(program(kProperScopes), 1), 10.0f);
}
