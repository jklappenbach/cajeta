//
// CajetaXPU CPU backend (Increment 5C) — wave ops as SIMD via the LLVM Vector
// Function ABI.
//
// 5B made data-parallel CPU kernels SIMD by vectorizing a per-block work-item
// loop. But wave ops (Wave.reduceSum / shuffleSync / ballotSync / width) are
// cross-lane and LoopVectorize bails on them — they stayed width-1. 5C gives
// each wave op a SIMD *vector variant* (a `vector-function-abi-variant` on its
// scalar stub) and forces the work-item loop to vectorize at the host's native
// width W, so LoopVectorize substitutes the variant: the wave becomes W SIMD
// lanes. Divergent (if-guarded) uses get a *masked* variant operating on active
// lanes only — GPU active-mask semantics. `width()` (no argument, so no legal
// VFABI variant) is rewritten to the constant W.
//
// Non-degrading by construction: a wave op lowers to a scalar call with the
// variant attribute; if vectorization fires it uses the variant (wave = W), if
// not the scalar width-1 stub runs — always correct.
//
// The execution tests are width-agnostic: each kernel probes Wave.width() for
// the true W on this host, then verifies the wave result against it (rather than
// hardcoding 16) — encoding the contract that the width is queried, never
// assumed. The IR test proves the SIMD substitution. GPU-free.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using cajeta::Compiler;
using cajeta::EmitMode;
using cajeta::XpuBackend;
using cajeta_test::CajetaJit;

namespace {

namespace fs = std::filesystem;

// One class, four wave kernels + self-verifying host drivers. Each run* method
// returns the detected wave width W on success, or a small failure code
// (< 8, or 100 + offending index) so a green assert means "ran SIMD AND the
// cross-lane math is correct at the real width".
const char* kWaveSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Wave;
public class M {
    @Kernel
    public static void widthk(Buffer<uint32> out) {
        uint32 t = Thread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void sumk(Buffer<uint32> out, Buffer<uint32> in) {
        uint32 t = Thread.globalIdX();
        out[t] = Wave.reduceSum(in[t]);
    }
    @Kernel
    public static void divsumk(Buffer<uint32> out, Buffer<uint32> in, uint32 active) {
        uint32 t = Thread.globalIdX();
        if (t < active) { out[t] = Wave.reduceSum(in[t]); }
    }
    @Kernel
    public static void ballotk(Buffer<uint32> out) {
        uint32 t = Thread.globalIdX();
        out[t] = (uint32) Wave.ballotSync(t < 4);
    }
    @Kernel
    public static void shufflek(Buffer<uint32> out, Buffer<uint32> in) {
        uint32 t = Thread.globalIdX();
        out[t] = Wave.shuffleSync(in[t], 0);
    }
    @Kernel
    public static void lanek(Buffer<uint32> out) {
        uint32 t = Thread.globalIdX();
        out[t] = Wave.laneId();
    }
    @Kernel
    public static void firstk(Buffer<uint32> out) {
        uint32 t = Thread.globalIdX();
        if (Wave.isFirstLane()) { out[t] = 1; } else { out[t] = 0; }
    }

    // Probe the wave width on this host (every lane writes W).
    public static uint32 probeWidth() {
        uint32 n = 256;
        uint32[] h = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { h[i] = 0; }
        Buffer<uint32> b = heap Buffer<uint32>(n);
        Stream s = Stream.current();
        widthk.launch(s, grid: [1], block: [256])(b);
        s.sync();
        b.download(h);
        return h[0];
    }

    public static uint32 runWidth() {
        uint32 n = 256;
        uint32[] h = new uint32[n];
        Buffer<uint32> b = heap Buffer<uint32>(n);
        Stream s = Stream.current();
        widthk.launch(s, grid: [1], block: [256])(b);
        s.sync();
        b.download(h);
        uint32 w = h[0];
        if (w < 2) { return 0; }
        for (uint32 i = 0; i < n; i = i + 1) {
            if (h[i] != w) { return 1; }
        }
        return w;
    }

    public static uint32 runReduceSum() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hin = new uint32[n];
        uint32[] hout = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = 1; hout[i] = 0; }
        Buffer<uint32> bin = heap Buffer<uint32>(n);
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        bin.upload(hin);
        Stream s = Stream.current();
        sumk.launch(s, grid: [1], block: [256])(bout, bin);
        s.sync();
        bout.download(hout);
        // Every lane gets its wave's sum of ones = W.
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != w) { return 100 + i; }
        }
        return w;
    }

    // Divergent reduceSum: only the first `active` work-items run the op. The
    // last (partial) wave reduces over its ACTIVE lanes only — the masked
    // variant. Inactive lanes leave out[] at its initial 0.
    public static uint32 runDivergentReduceSum() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32 active = 200;
        uint32[] hin = new uint32[n];
        uint32[] hout = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = 1; hout[i] = 0; }
        Buffer<uint32> bin = heap Buffer<uint32>(n);
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        bin.upload(hin);
        bout.upload(hout);
        Stream s = Stream.current();
        divsumk.launch(s, grid: [1], block: [256])(bout, bin, active);
        s.sync();
        bout.download(hout);
        for (uint32 t = 0; t < n; t = t + 1) {
            uint32 base = (t / w) * w;
            uint32 cnt = 0;
            for (uint32 l = 0; l < w; l = l + 1) {
                if (base + l < active) { cnt = cnt + 1; }
            }
            uint32 expected = 0;
            if (t < active) { expected = cnt; }   // active lanes summed the 1s
            if (hout[t] != expected) { return 100 + t; }
        }
        return w;
    }

    public static uint32 runBallot() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hout = new uint32[n];
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        Stream s = Stream.current();
        ballotk.launch(s, grid: [1], block: [256])(bout);
        s.sync();
        bout.download(hout);
        // ballot(t < 4) is wave-uniform: every lane of a wave sees the same
        // mask, bit l set iff lane l's global index (base + l) < 4.
        for (uint32 t = 0; t < n; t = t + 1) {
            uint32 base = (t / w) * w;
            uint32 mask = 0;
            for (uint32 l = 0; l < w; l = l + 1) {
                if (base + l < 4) { mask = mask + (1 << l); }
            }
            if (hout[t] != mask) { return 100 + t; }
        }
        return w;
    }

    public static uint32 runLaneId() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hout = new uint32[n];
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        Stream s = Stream.current();
        lanek.launch(s, grid: [1], block: [256])(bout);
        s.sync();
        bout.download(hout);
        // laneId() = work-item index within its wave = t mod W.
        for (uint32 t = 0; t < n; t = t + 1) {
            if (hout[t] != t % w) { return 100 + t; }
        }
        return w;
    }

    public static uint32 runFirstLane() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hout = new uint32[n];
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        Stream s = Stream.current();
        firstk.launch(s, grid: [1], block: [256])(bout);
        s.sync();
        bout.download(hout);
        // isFirstLane() is true exactly when laneId() == 0, i.e. t mod W == 0.
        for (uint32 t = 0; t < n; t = t + 1) {
            uint32 expected = 0;
            if (t % w == 0) { expected = 1; }
            if (hout[t] != expected) { return 100 + t; }
        }
        return w;
    }

    public static uint32 runShuffle() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hin = new uint32[n];
        uint32[] hout = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = i + 100; hout[i] = 0; }
        Buffer<uint32> bin = heap Buffer<uint32>(n);
        Buffer<uint32> bout = heap Buffer<uint32>(n);
        bin.upload(hin);
        Stream s = Stream.current();
        shufflek.launch(s, grid: [1], block: [256])(bout, bin);
        s.sync();
        bout.download(hout);
        // shuffleSync(in[t], 0): every lane reads lane 0 of its wave = in[base].
        for (uint32 t = 0; t < n; t = t + 1) {
            uint32 base = (t / w) * w;
            uint32 expected = base + 100;
            if (hout[t] != expected) { return 100 + t; }
        }
        return w;
    }
}
)CJ";

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

bool isPowerOfTwo(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }

// Run a self-verifying host driver; assert it returned the wave width (success).
unsigned runWaveDriver(const char* method) {
    auto jit = CajetaJit::compile(kWaveSource, "test.M", cpuOptions());
    EXPECT_NE(jit, nullptr);
    if (!jit) return 0;
    auto fn = jit->lookup<unsigned (*)()>(method);
    EXPECT_NE(fn, nullptr);
    if (!fn) return 0;
    unsigned w = fn();
    EXPECT_GT(w, 2u) << method << " returned failure code " << w
                     << " (< 8: setup; 100+i: wrong result at lane i-100)";
    EXPECT_TRUE(isPowerOfTwo(w)) << "wave width " << w << " not a power of two";
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx512f")) EXPECT_EQ(w, 16u);
    else if (__builtin_cpu_supports("avx2")) EXPECT_EQ(w, 8u);
#endif
    return w;
}

std::string compileToIr(const char* source, const std::string& entry) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xpu_wavesimd_" + std::to_string(rng()));
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

} // namespace

// reduceSum: each wave's sum, computed at the real host width (all-ones → W).
TEST(XpuCpuWaveSimdTests, reduceSumWaveIsSimdWidth) {
    runWaveDriver("runReduceSum");
}

// width(): every lane sees the architectural wave width W (the queryable source
// of truth — folded to a constant in the vectorized kernel).
TEST(XpuCpuWaveSimdTests, widthReportsSimdWidth) {
    runWaveDriver("runWidth");
}

// Divergent reduceSum (if-guarded): the partial last wave reduces over its
// ACTIVE lanes only via the masked VFABI variant — GPU active-mask semantics.
TEST(XpuCpuWaveSimdTests, divergentReduceSumIsActiveLaneMasked) {
    runWaveDriver("runDivergentReduceSum");
}

// ballotSync: the per-lane predicate packs into a wave-uniform bitmask.
TEST(XpuCpuWaveSimdTests, ballotPacksLanePredicateMask) {
    runWaveDriver("runBallot");
}

// shuffleSync: every lane reads a chosen source lane of its wave (lane 0 here).
TEST(XpuCpuWaveSimdTests, shuffleReadsSourceLane) {
    runWaveDriver("runShuffle");
}

// laneId(): each work-item's index within its wave = t mod W — the other half of
// the queryable environment surface (with width()).
TEST(XpuCpuWaveSimdTests, laneIdIsIndexWithinWave) {
    runWaveDriver("runLaneId");
}

// isFirstLane(): the width-agnostic cooperation guard — true once per wave, at
// lane 0 (a divergent use, exercising the masked path too).
TEST(XpuCpuWaveSimdTests, isFirstLaneTrueOncePerWave) {
    runWaveDriver("runFirstLane");
}

// Substitution: the block wrapper's work-item loop vectorizes and LoopVectorize
// replaces the scalar reduceSum call with its SIMD variant — visible as the
// VFABI attribute and the `llvm.vector.reduce.add` the variant lowers to (the
// width-1 scalar path never emits a vector reduce).
TEST(XpuCpuWaveSimdTests, reduceSumEmitsVfabiVariantInBlockWrapper) {
    std::string ir = compileToIr(kWaveSource, "test.M.sumk");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("__cajeta_xpu_cpu_block.sumk"), std::string::npos) << ir;
    EXPECT_NE(ir.find("vector-function-abi-variant"), std::string::npos) << ir;
    EXPECT_NE(ir.find("__cajeta_xpu_wave_reduce_sum_u32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.vector.reduce.add"), std::string::npos)
        << "expected a vectorized wave-reduce in the block wrapper\n" << ir;
}
