//
// CajetaXPU CPU backend (Increment 6) — workgroup barriers via work-item loop
// fission, run end to end and GPU-FREE.
//
// A `@Kernel` with `Barrier.workgroup()` can't run as one work-item loop: a
// barrier means every work-item of the block must reach it before any proceeds.
// POCL-style fission splits the kernel at each barrier into regions and wraps
// each in its own loop over the block; values that live across a barrier get
// per-work-item context arrays, and shared memory becomes a per-block buffer.
// Because a per-block wrapper call runs on one thread, the serialized region
// loops honor the barrier for free.
//
// These tests JIT-compile a barrier kernel `--xpu-backend=cpu` and run it,
// verifying the result for EVERY work-item (not just the last) — the proof that
// the split regions ran the whole block in order.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// Compile `source` to IR with --xpu-backend=cpu; return the kernel-module IR.
std::string compileToIr(const char* source, const std::string& entry) {
    namespace fs = std::filesystem;
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xpu_barrier_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "test");
    std::ofstream(base / "src" / "test" / "M.cajeta") << source;
    fs::create_directories(base / "build");
    cajeta::Compiler compiler;
    compiler.setEmitMode(cajeta::EmitMode::IR);
    compiler.setXpuBackend(cajeta::XpuBackend::Cpu);
    compiler.compile(entry, (base / "src").string(), (base / "build").string());
    std::string ir;
    for (auto& e : fs::recursive_directory_iterator(base / "build"))
        if (e.is_regular_file() && e.path().extension() == ".ll"
            && e.path().filename() != "cajeta.runtime.__stdlib__.ll") {
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf(); ir = ss.str();
        }
    fs::remove_all(base);
    return ir;
}

// Two straight-line regions split by one barrier. Region A writes a[t]=t for all
// t; region B writes b[t]=t+100 for all t. `t` lives across the barrier (→ a
// per-work-item context array). run() verifies both halves for every work-item
// and returns a success sentinel, or 100+i / 1000+i on the first wrong lane.
const char* kTwoStageSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
public class M {
    @Kernel
    public static void twostage(Buffer<uint32> a, Buffer<uint32> b) {
        uint32 t = Thread.globalIdX();
        a[t] = t;
        Barrier.workgroup();
        b[t] = t + 100;
    }
    public static uint32 run() {
        uint32 n = 256;
        uint32[] ha = new uint32[n];
        uint32[] hb = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { ha[i] = 0; hb[i] = 0; }
        Buffer<uint32> a = heap Buffer<uint32>(n);
        Buffer<uint32> b = heap Buffer<uint32>(n);
        a.upload(ha);
        b.upload(hb);
        Stream s = Stream.current();
        twostage.launch(s, grid: [1], block: [256])(a, b);
        s.sync();
        a.download(ha);
        b.download(hb);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (ha[i] != i) { return 100 + i; }
            if (hb[i] != i + 100) { return 1000 + i; }
        }
        return 12345;
    }
}
)CJ";

// Shared memory across a barrier: stage in[t] into a shared tile, barrier, then
// read a DIFFERENT lane (tile[255-t]). A correct result for every t proves all
// work-items staged before any read — i.e. the barrier and the per-block shared
// buffer work. out[t] == in[255-t] == 255-t.
const char* kSharedStageSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void stageback(Buffer<uint32> out, Buffer<uint32> in) {
        Shared<uint32> tile = shared uint32[256];
        uint32 t = Thread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        out[t] = tile[255 - t];
    }
    public static uint32 run() {
        uint32 n = 256;
        uint32[] hin = new uint32[n];
        uint32[] hout = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = i; hout[i] = 0; }
        Buffer<uint32> in = heap Buffer<uint32>(n);
        Buffer<uint32> out = heap Buffer<uint32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        stageback.launch(s, grid: [1], block: [256])(out, in);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != 255 - i) { return 100 + i; }
        }
        return 12345;
    }
}
)CJ";

// The canonical shared-memory tree reduction: a barrier inside a uniform loop.
// Stage in[t] into a shared tile, barrier, then halve-and-add with a barrier
// each step; lane 0 writes the block's sum. The uniform `for (s)` loop stays an
// outer scalar scaffold; region B (the add) is a work-item loop nested inside.
// in[i]=i over a 256 block ⇒ out[0] = 0+1+…+255 = 32640.
const char* kReduceSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Workgroup;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void reduce(Buffer<int32> out, Buffer<int32> in, uint32 n) {
        Shared<int32> tile = shared int32[256];
        uint32 t = Thread.x();
        uint32 g = Thread.globalIdX();
        if (g < n) { tile[t] = in[g]; } else { tile[t] = 0; }
        Barrier.workgroup();
        for (uint32 s = 128; s > 0; s >>= 1) {
            if (t < s) { tile[t] += tile[t + s]; }
            Barrier.workgroup();
        }
        if (t == 0) { out[Workgroup.x()] = tile[0]; }
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hin = new int32[n];
        int32[] hout = new int32[1];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; }
        hout[0] = 0;
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(1);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        reduce.launch(s, grid: [1], block: [256])(out, in, n);
        s.sync();
        out.download(hout);
        return hout[0];
    }
    // 32 blocks × 256 = 8192 work-items (> the 4096 parallel threshold), so
    // blocks run on different pthread workers concurrently. Each block reduces
    // its own 256-slice; per-block shared buffers must not alias across workers.
    // block b sums [b*256, b*256+256) = 65536*b + 32640. Returns 1 on success.
    public static int32 runMultiBlock() {
        uint32 blocks = 32;
        uint32 n = blocks * 256;
        int32[] hin = new int32[n];
        int32[] hout = new int32[blocks];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; }
        for (uint32 b = 0; b < blocks; b = b + 1) { hout[b] = 0; }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(blocks);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        reduce.launch(s, grid: [32], block: [256])(out, in, n);
        s.sync();
        out.download(hout);
        for (uint32 b = 0; b < blocks; b = b + 1) {
            int32 expected = (int32) (65536 * b + 32640);
            if (hout[b] != expected) { return (int32) (100 + b); }
        }
        return 1;
    }
}
)CJ";

} // namespace

// One barrier, two regions: both halves run for every work-item, with `t`
// carried across the barrier through its context array.
TEST(XpuCpuBarrierExecTests, twoStraightLineRegionsRunWholeBlock) {
    auto jit = CajetaJit::compile(kTwoStageSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<unsigned (*)()>("run");
    ASSERT_NE(fn, nullptr);
    unsigned r = fn();
    EXPECT_EQ(r, 12345u) << "fail code " << r
        << " (100+i: a[i]!=i; 1000+i: b[i]!=i+100)";
}

// Per-block shared memory across a barrier, read cross-lane.
TEST(XpuCpuBarrierExecTests, sharedMemoryStagedAcrossBarrier) {
    auto jit = CajetaJit::compile(kSharedStageSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<unsigned (*)()>("run");
    ASSERT_NE(fn, nullptr);
    unsigned r = fn();
    EXPECT_EQ(r, 12345u) << "fail code " << r << " (100+i: out[i] != 255-i)";
}

// A barrier INSIDE a uniform loop — the tree reduction. The s-loop stays an
// outer scalar loop; the work-item loop nests inside. sum(0..255) = 32640.
TEST(XpuCpuBarrierExecTests, sharedTreeReductionInUniformLoop) {
    auto jit = CajetaJit::compile(kReduceSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 32640);
}

// Many blocks across pthread workers: per-block shared buffers must not alias.
TEST(XpuCpuBarrierExecTests, multiBlockReductionNoSharedAliasing) {
    auto jit = CajetaJit::compile(kReduceSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("runMultiBlock");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 1) << "fail code " << r << " (100+b: block b's partial wrong)";
}

// Guardrail: a barrier under work-item-divergent control flow is GPU-undefined.
// Fission rejects it (XPU-N02) and the kernel falls back to the host stub —
// compilation succeeds (no crash) and emits no fission wrapper.
TEST(XpuCpuBarrierExecTests, divergentBarrierFallsBackCleanly) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "import cajeta.xpu.core.Barrier;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void divergent(Buffer<uint32> a) {\n"
        "        uint32 t = Thread.globalIdX();\n"
        "        if (t < 4) { Barrier.workgroup(); a[t] = t; }\n"
        "    }\n"
        "}\n";
    std::string ir = compileToIr(src, "test.M.divergent");
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(ir.find("__cajeta_xpu_cpu_block.divergent"), std::string::npos)
        << "divergent barrier should fall back, not fission\n";
}
