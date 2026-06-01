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

#include <cctype>
#include <cstdlib>
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

// Increment 7 — MULTIPLE barriers inside one uniform loop. Ping-pong between two
// shared tiles: each iteration reads `a` into `b` (a circular neighbour sum),
// barrier, copies `b` back to `a`, barrier. Two regions per loop body, split by
// the in-loop barrier; the loop stays the outer scalar scaffold. `run()`
// computes the same recurrence on the host and compares every lane.
const char* kPingPongSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void pingpong(Buffer<int32> out, Buffer<int32> in, uint32 k) {
        Shared<int32> a = shared int32[256];
        Shared<int32> b = shared int32[256];
        uint32 t = Thread.x();
        a[t] = in[t];
        Barrier.workgroup();
        for (uint32 i = 0; i < k; i = i + 1) {
            b[t] = a[t] + a[(t + 1) & 255];
            Barrier.workgroup();
            a[t] = b[t];
            Barrier.workgroup();
        }
        out[t] = a[t];
    }
    public static int32 run() {
        uint32 n = 256;
        uint32 k = 3;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        int32[] ra = new int32[n];
        int32[] rb = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; ra[i] = (int32) i; }
        for (uint32 it = 0; it < k; it = it + 1) {
            for (uint32 i = 0; i < n; i = i + 1) { rb[i] = ra[i] + ra[(i + 1) & 255]; }
            for (uint32 i = 0; i < n; i = i + 1) { ra[i] = rb[i]; }
        }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        pingpong.launch(s, grid: [1], block: [256])(out, in, k);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != ra[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Increment 7 — a per-work-item LOCAL carried across an in-loop barrier. Same
// recurrence, but the intermediate is a scalar local `x` (not shared memory):
// `x = a[t]+a[(t+1)&255]` is computed in the loop's first region and READ in the
// second region after the barrier, so `x` must get a per-work-item context array
// (written then read within the same iteration). Proves context arrays work for
// values that cross a barrier *inside* a loop, not just the straight-line case.
const char* kLocalCarrySource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void localcarry(Buffer<int32> out, Buffer<int32> in, uint32 k) {
        Shared<int32> a = shared int32[256];
        uint32 t = Thread.x();
        a[t] = in[t];
        Barrier.workgroup();
        for (uint32 i = 0; i < k; i = i + 1) {
            int32 x = a[t] + a[(t + 1) & 255];
            Barrier.workgroup();
            a[t] = x;
            Barrier.workgroup();
        }
        out[t] = a[t];
    }
    public static int32 run() {
        uint32 n = 256;
        uint32 k = 4;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        int32[] ra = new int32[n];
        int32[] rb = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; ra[i] = (int32) i; }
        for (uint32 it = 0; it < k; it = it + 1) {
            for (uint32 i = 0; i < n; i = i + 1) { rb[i] = ra[i] + ra[(i + 1) & 255]; }
            for (uint32 i = 0; i < n; i = i + 1) { ra[i] = rb[i]; }
        }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        localcarry.launch(s, grid: [1], block: [256])(out, in, k);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != ra[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Increment 7 — a per-work-item REGISTER accumulator carried across a uniform
// loop's back-edge (the Inc 6 reduction sidesteps this by keeping state in
// shared memory). `acc` is a scalar local: init to 0 before the loop, += a
// shared-tile element each iteration, written out after. It lives across the
// loop barrier AND across iterations, so it must be a context array whose
// per-work-item slot persists across the outer scalar loop. out[t] = sum over
// i in [0,k) of tile[(t+i)&255], with tile[j]=in[j]=j.
const char* kAccumSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void accum(Buffer<int32> out, Buffer<int32> in, uint32 k) {
        Shared<int32> tile = shared int32[256];
        uint32 t = Thread.x();
        int32 acc = 0;
        tile[t] = in[t];
        Barrier.workgroup();
        for (uint32 i = 0; i < k; i = i + 1) {
            acc = acc + tile[(t + i) & 255];
            Barrier.workgroup();
        }
        out[t] = acc;
    }
    public static int32 run() {
        uint32 n = 256;
        uint32 k = 5;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        int32[] ref = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; }
        for (uint32 t = 0; t < n; t = t + 1) {
            int32 acc = 0;
            for (uint32 i = 0; i < k; i = i + 1) { acc = acc + hin[(t + i) & 255]; }
            ref[t] = acc;
        }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        accum.launch(s, grid: [1], block: [256])(out, in, k);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != ref[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Increment 8 — a barrier inside a loop NESTED in another loop (the one-level
// restriction lifted). Same circular-neighbour-sum recurrence as localcarry,
// applied ki*kj times via a doubly-nested loop with the in-loop barrier in the
// INNER loop. Both loops stay outer scalar scaffolds; the inner body's two
// regions are work-item loops nested two deep. The register temp `v` crosses the
// inner barrier (context array). out matches the host recurrence applied ki*kj
// times for every lane.
const char* kNestedSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void nested(Buffer<int32> out, Buffer<int32> in,
                              uint32 ki, uint32 kj) {
        Shared<int32> tile = shared int32[256];
        uint32 t = Thread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        for (uint32 i = 0; i < ki; i = i + 1) {
            for (uint32 j = 0; j < kj; j = j + 1) {
                int32 v = tile[t] + tile[(t + 1) & 255];
                Barrier.workgroup();
                tile[t] = v;
                Barrier.workgroup();
            }
        }
        out[t] = tile[t];
    }
    public static int32 run() {
        uint32 n = 256;
        uint32 ki = 2;
        uint32 kj = 3;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        int32[] ra = new int32[n];
        int32[] rb = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; ra[i] = (int32) i; }
        for (uint32 it = 0; it < ki * kj; it = it + 1) {
            for (uint32 i = 0; i < n; i = i + 1) { rb[i] = ra[i] + ra[(i + 1) & 255]; }
            for (uint32 i = 0; i < n; i = i + 1) { ra[i] = rb[i]; }
        }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        nested.launch(s, grid: [1], block: [256])(out, in, ki, kj);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != ra[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Increment 8 — an outer loop holding BOTH a direct barrier-region AND a nested
// barrier-loop: each outer iteration applies the recurrence once at the outer
// level (read; barrier; write; barrier) then kj more times in the inner loop, so
// the outer loop level is a sequence `region · barrier · region · barrier ·
// subloop`. Stresses the walk handling direct regions and a subloop at one level.
// Recurrence R(tile)[t] = tile[t] + tile[(t+1)&255] applied ki*(kj+1) times.
const char* kNested2Source = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void nested2(Buffer<int32> out, Buffer<int32> in,
                               uint32 ki, uint32 kj) {
        Shared<int32> tile = shared int32[256];
        uint32 t = Thread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        for (uint32 i = 0; i < ki; i = i + 1) {
            int32 pre = tile[(t + 1) & 255];
            Barrier.workgroup();
            tile[t] = tile[t] + pre;
            Barrier.workgroup();
            for (uint32 j = 0; j < kj; j = j + 1) {
                int32 v = tile[t] + tile[(t + 1) & 255];
                Barrier.workgroup();
                tile[t] = v;
                Barrier.workgroup();
            }
        }
        out[t] = tile[t];
    }
    public static int32 run() {
        uint32 n = 256;
        uint32 ki = 2;
        uint32 kj = 2;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        int32[] ra = new int32[n];
        int32[] rb = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; ra[i] = (int32) i; }
        uint32 reps = ki * (kj + 1);
        for (uint32 it = 0; it < reps; it = it + 1) {
            for (uint32 i = 0; i < n; i = i + 1) { rb[i] = ra[i] + ra[(i + 1) & 255]; }
            for (uint32 i = 0; i < n; i = i + 1) { ra[i] = rb[i]; }
        }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        nested2.launch(s, grid: [1], block: [256])(out, in, ki, kj);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != ra[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Increment 9 — wave ops + a barrier composed in one kernel: the canonical
// two-level block reduction. Each work-item's value is summed within its wave
// (Wave.reduceSum → W SIMD lanes), the wave leaders stage their partial into
// shared memory, a barrier, then lane 0 sums the per-wave partials. The block
// total is INDEPENDENT of the wave width W, so the expected result is just
// sum(0..255) = 32640 regardless of whether the host wave is 16/8/4 wide —
// encoding "query the width, never hardcode" while proving wave SIMD and the
// barrier coexist (the wave op vectorizes each fission region at W).
const char* kBlockReduceSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Wave;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void blockReduce(Buffer<int32> out, Buffer<int32> in) {
        Shared<int32> partials = shared int32[256];
        uint32 t = Thread.x();
        int32 wsum = Wave.reduceSum(in[t]);
        uint32 w = Wave.width();
        if (t % w == 0) { partials[t / w] = wsum; }
        Barrier.workgroup();
        if (t == 0) {
            int32 total = 0;
            uint32 nwaves = 256 / w;
            for (uint32 i = 0; i < nwaves; i = i + 1) { total = total + partials[i]; }
            out[0] = total;
        }
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
        blockReduce.launch(s, grid: [1], block: [256])(out, in);
        s.sync();
        out.download(hout);
        return hout[0];
    }
}
)CJ";

// Increment 9 — width-agnostic membership check that wave VALUES (not just an
// associative total) compose with a barrier. With in[t] = 1, a wave reduce sums
// W ones, so every lane gets exactly W; staged through shared + a barrier, out[t]
// must equal the probed wave width for all t (full waves, block 256 ÷ W). `run()`
// first launches a width probe to learn W, then checks every lane — so it holds
// at 16/8/4 without hardcoding.
const char* kWaveBarMembershipSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Wave;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void widthk(Buffer<uint32> out) {
        uint32 t = Thread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void wavebar(Buffer<int32> out, Buffer<int32> in) {
        Shared<int32> tile = shared int32[256];
        uint32 t = Thread.x();
        int32 s = Wave.reduceSum(in[t]);
        tile[t] = s;
        Barrier.workgroup();
        out[t] = tile[t];
    }
    public static int32 run() {
        uint32 n = 256;
        uint32[] hw = new uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hw[i] = 0; }
        Buffer<uint32> wbuf = heap Buffer<uint32>(n);
        wbuf.upload(hw);
        Stream s = Stream.current();
        widthk.launch(s, grid: [1], block: [256])(wbuf);
        s.sync();
        wbuf.download(hw);
        int32 W = (int32) hw[0];
        if (W < 1) { return -1; }

        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = 1; hout[i] = 0; }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        wavebar.launch(s, grid: [1], block: [256])(out, in);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != W) { return (int32) (1000 + i); }
        }
        return W;
    }
}
)CJ";

// Dynamic (runtime-sized) shared memory + a barrier: `shared int32[n]` with `n` a
// runtime parameter lowers to an external unsized addrspace(3) global; the launch
// passes its byte count via `sharedBytes:`, and the per-block wrapper allocas that
// many bytes. Otherwise the canonical tree reduction. in[i]=i over a 256 block,
// sharedBytes = 256*4 ⇒ out[0] = 32640.
const char* kDynSharedSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Workgroup;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void dynreduce(Buffer<int32> out, Buffer<int32> in, uint32 n) {
        Shared<int32> tile = shared int32[n];
        uint32 t = Thread.x();
        if (t < n) { tile[t] = in[t]; } else { tile[t] = 0; }
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
        dynreduce.launch(s, grid: [1], block: [256], sharedBytes: [1024])(out, in, n);
        s.sync();
        out.download(hout);
        return hout[0];
    }
}
)CJ";

// 2D/3D launch stage 4 — a barrier kernel over a 2-D BLOCK. An 8×8 tile is staged
// into shared memory, a barrier, then each thread reads the TRANSPOSED element
// (staged by a different thread → the barrier is load-bearing) and writes the
// transpose out. tx/ty are per-work-item locals that cross the barrier (context
// arrays, indexed by the linearized work-item index). in[i]=i ⇒ out is the
// transpose: out[ty*8+tx] = in[tx*8+ty] = tx*8+ty. Proves fission runs the full
// 3-D work-item nest (tid.x AND tid.y) inside the barrier regions.
const char* kTranspose2dSource = R"CJ(
package test;
import cajeta.xpu.core.Buffer;
import cajeta.xpu.core.Stream;
import cajeta.xpu.core.Thread;
import cajeta.xpu.core.Barrier;
import cajeta.xpu.core.Shared;
public class M {
    @Kernel
    public static void transpose2d(Buffer<int32> out, Buffer<int32> in) {
        Shared<int32> tile = shared int32[64];
        uint32 tx = Thread.x();
        uint32 ty = Thread.y();
        tile[ty * 8 + tx] = in[ty * 8 + tx];
        Barrier.workgroup();
        out[ty * 8 + tx] = tile[tx * 8 + ty];
    }
    public static int32 run() {
        uint32 n = 64;
        int32[] hin = new int32[n];
        int32[] hout = new int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; }
        Buffer<int32> in = heap Buffer<int32>(n);
        Buffer<int32> out = heap Buffer<int32>(n);
        in.upload(hin);
        out.upload(hout);
        Stream s = Stream.current();
        transpose2d.launch(s, grid: [1], block: [8, 8])(out, in);
        s.sync();
        out.download(hout);
        for (uint32 ty = 0; ty < 8; ty = ty + 1) {
            for (uint32 tx = 0; tx < 8; tx = tx + 1) {
                if (hout[ty * 8 + tx] != (int32) (tx * 8 + ty)) {
                    return (int32) (100 + ty * 8 + tx);
                }
            }
        }
        return 777;
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

// Guardrail (Increment 8): nesting is supported, but a barrier in a loop whose
// trip count is work-item-dependent is GPU-undefined (work-items would hit
// different barrier counts → deadlock). Even nested, it must fall back cleanly —
// removing the one-level cap must not open a miscompile hole here.
TEST(XpuCpuBarrierExecTests, nestedTidDependentTripCountFallsBack) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "import cajeta.xpu.core.Barrier;\n"
        "import cajeta.xpu.core.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void nesttid(Buffer<int32> out, Buffer<int32> in,\n"
        "                               uint32 ki) {\n"
        "        Shared<int32> tile = shared int32[256];\n"
        "        uint32 t = Thread.x();\n"
        "        tile[t] = in[t];\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 i = 0; i < ki; i = i + 1) {\n"
        "            for (uint32 j = 0; j < t; j = j + 1) {\n"
        "                tile[t] = tile[t] + 1;\n"
        "                Barrier.workgroup();\n"
        "            }\n"
        "        }\n"
        "        out[t] = tile[t];\n"
        "    }\n"
        "}\n";
    std::string ir = compileToIr(src, "test.M.nesttid");
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(ir.find("__cajeta_xpu_cpu_block.nesttid"), std::string::npos)
        << "tid-dependent inner trip count must fall back, not fission\n";
}

// Increment 7: two barriers inside one uniform loop (shared-memory ping-pong).
// The loop body splits into two work-item-loop regions; the loop stays the outer
// scalar scaffold. Result matches the host recurrence for every lane.
TEST(XpuCpuBarrierExecTests, multipleBarriersInOneLoop) {
    auto jit = CajetaJit::compile(kPingPongSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != reference[i])";
}

// Increment 7: a per-work-item local carried across an in-loop barrier — proves
// context arrays work for a value that crosses a barrier *inside* a loop.
TEST(XpuCpuBarrierExecTests, localCarriedAcrossInLoopBarrier) {
    auto jit = CajetaJit::compile(kLocalCarrySource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != reference[i])";
}

// Increment 7: a per-work-item REGISTER accumulator carried across a uniform
// loop's back-edge. The context array (allocated once in the wrapper entry,
// indexed by work-item) persists across the outer scalar loop's iterations, so
// the accumulation carries correctly without shared memory for the accumulator.
TEST(XpuCpuBarrierExecTests, registerAccumulatorAcrossLoopBackEdge) {
    auto jit = CajetaJit::compile(kAccumSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != reference[i])";
}

// Increment 8: a barrier inside a loop NESTED in another loop. Both loops stay
// outer scalar scaffolds; the inner body's regions are work-item loops nested
// two deep. Matches the host recurrence applied ki*kj times for every lane.
TEST(XpuCpuBarrierExecTests, nestedUniformLoopsWithBarrier) {
    auto jit = CajetaJit::compile(kNestedSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != reference[i])";
}

// Increment 8: an outer loop with a direct barrier-region AND a nested
// barrier-loop at the same level — the sequence region·barrier·region·barrier·
// subloop inside one uniform loop.
TEST(XpuCpuBarrierExecTests, nestedLoopsWithOuterLevelRegions) {
    auto jit = CajetaJit::compile(kNested2Source, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != reference[i])";
}

// Increment 9: wave ops + a barrier composed — the two-level block reduction.
// out[0] = sum(0..255) = 32640, independent of the runtime wave width.
TEST(XpuCpuBarrierExecTests, waveReduceWithBarrierBlockSum) {
    auto jit = CajetaJit::compile(kBlockReduceSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 32640);
}

// Increment 9: wave VALUES compose with a barrier — in[t]=1 ⇒ out[t]==W (the
// probed wave width) for every lane, proving per-wave membership survives
// fission, not just an associative total. Returns W (>=2 on a SIMD host).
TEST(XpuCpuBarrierExecTests, waveReduceWithBarrierMembership) {
    auto jit = CajetaJit::compile(kWaveBarMembershipSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_GE(r, 2) << "fail code " << r << " (1000+i: out[i] != W; -1: bad probe)";
}

// Dynamic (runtime-sized) shared memory + a barrier: the wrapper allocas the
// launch's `sharedBytes:` count per block. Tree reduction ⇒ out[0]=32640.
TEST(XpuCpuBarrierExecTests, dynamicSharedMemoryWithBarrier) {
    auto jit = CajetaJit::compile(kDynSharedSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 32640);
}

// 2D/3D launch stage 4: a barrier kernel over a 2-D block (8×8 shared-memory
// transpose). Fission runs the full 3-D work-item nest inside the regions, and
// tx/ty cross the barrier via context arrays indexed by the linearized index.
TEST(XpuCpuBarrierExecTests, barrierKernelOver2dBlock) {
    auto jit = CajetaJit::compile(kTranspose2dSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != transpose[i])";
}

// ----------------------------------------------------------------------------
// Structural (emit-level) assertions on the fission output.
//
// The execution tests above prove the wrapper *behaves* right; these pin the
// *shape* the plan promised: one counted work-item loop per region, the uniform
// loop kept as a single outer scalar scaffold (work-item loops nested inside),
// no addrspace(3) shared global left behind (replaced by a per-block stack
// buffer), and per-work-item context arrays for values that cross a barrier.
//
// Region/loop *counts* are only stable BEFORE LoopVectorize — it rotates and
// folds the per-region preheaders (region A's `wi.ph` merges into `entry`),
// so the `CAJETA_XPU_CPU_NO_VECTORIZE` seam captures the wrapper pre-vectorize
// for the count/nesting checks. A final test confirms the SHIPPED (vectorized)
// IR still carries the invariants that survive vectorization.
// ----------------------------------------------------------------------------

namespace {

struct ScopedEnv {
    std::string key;
    ScopedEnv(const char* k, const char* v) : key(k) { ::setenv(k, v, 1); }
    ~ScopedEnv() { ::unsetenv(key.c_str()); }
};

// The body text of `define ... @<name>(...) { ... }`, or "" if absent.
std::string functionBody(const std::string& ir, const std::string& name) {
    auto d = ir.find("define ");
    while (d != std::string::npos) {
        auto open = ir.find('(', d);
        auto nm = ir.rfind('@' + name + '(', open);
        if (nm != std::string::npos && nm > d && nm < open) {
            auto brace = ir.find('{', open);
            auto end = ir.find("\n}", brace);
            if (brace != std::string::npos && end != std::string::npos)
                return ir.substr(brace, end - brace + 2);
        }
        d = ir.find("\ndefine ", d + 1);
        if (d != std::string::npos) d += 1;
    }
    return "";
}

// Count basic-block label definitions whose name is `base` + an optional numeric
// uniquifying suffix (LLVM appends digits on name collisions): wi.ph, wi.ph2, …
int countLabels(const std::string& body, const std::string& base) {
    int n = 0;
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string lbl = line.substr(0, c);
        if (lbl.rfind(base, 0) != 0) continue;
        bool ok = true;
        for (char ch : lbl.substr(base.size()))
            if (!std::isdigit((unsigned char) ch)) { ok = false; break; }
        if (ok) ++n;
    }
    return n;
}

int countSubstr(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (auto p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// True iff some `wi.ph*` preheader is entered from the uniform loop header
// (`preds = ... %for.head ...`) — i.e. a work-item loop nests inside it.
bool hasWorkItemLoopUnderUniformLoop(const std::string& body) {
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line))
        if (line.rfind("wi.ph", 0) == 0
            && line.find("%for.head") != std::string::npos)
            return true;
    return false;
}

} // namespace

// One barrier ⇒ exactly two straight-line regions ⇒ two work-item loops, each
// looping the whole block. No source loop (`for.head`), no shared global; `t`
// crosses the barrier so it gets a per-work-item context array; one ret.
TEST(XpuCpuBarrierEmitTests, twoStageWrapperHasTwoWorkItemLoops) {
    ScopedEnv noVec("CAJETA_XPU_CPU_NO_VECTORIZE", "1");
    std::string ir = compileToIr(kTwoStageSource, "test.M.twostage");
    ASSERT_FALSE(ir.empty());
    std::string body = functionBody(ir, "__cajeta_xpu_cpu_block.twostage");
    ASSERT_FALSE(body.empty()) << "no fission wrapper emitted";

    // Two regions, each a well-formed counted work-item loop (ph/head/latch).
    EXPECT_EQ(countLabels(body, "wi.ph"), 2);
    EXPECT_EQ(countLabels(body, "wi.head"), 2);
    EXPECT_EQ(countLabels(body, "wi.latch"), 2);
    EXPECT_EQ(countLabels(body, "for.head"), 0) << "no source loop expected";

    // Barriers consumed; no shared memory; `t` carried across via a ctx array.
    EXPECT_EQ(countSubstr(body, "call void @__cajeta_xpu_cpu_barrier"), 0);
    EXPECT_EQ(countSubstr(body, "alloca ["), 0) << "no shared buffer expected";
    EXPECT_EQ(countSubstr(ir, "addrspace(3) global"), 0);
    EXPECT_GE(countSubstr(body, "ctx = alloca"), 1) << "t crosses the barrier";
    EXPECT_EQ(countSubstr(body, "ret void"), 1) << "single exit (wrap.end)";
}

// The tree reduction: a barrier inside a uniform loop. The s-loop stays ONE
// outer scalar loop (`for.head` once); the loop-body region is a work-item loop
// nested inside it; the addrspace(3) shared global becomes a per-block stack
// buffer; multiple regions, each a well-formed work-item loop.
TEST(XpuCpuBarrierEmitTests, reductionNestsWorkItemLoopInUniformLoop) {
    ScopedEnv noVec("CAJETA_XPU_CPU_NO_VECTORIZE", "1");
    std::string ir = compileToIr(kReduceSource, "test.M.reduce");
    ASSERT_FALSE(ir.empty());
    std::string body = functionBody(ir, "__cajeta_xpu_cpu_block.reduce");
    ASSERT_FALSE(body.empty()) << "no fission wrapper emitted";

    // The uniform loop is preserved as exactly one outer scalar loop, NOT
    // fissioned into per-region work-item loops, and a work-item loop nests in.
    EXPECT_EQ(countLabels(body, "for.head"), 1) << "uniform loop kept as outer";
    EXPECT_TRUE(hasWorkItemLoopUnderUniformLoop(body))
        << "the loop-body region must be a work-item loop inside for.head";

    // Multiple regions, each a well-formed counted work-item loop.
    int ph = countLabels(body, "wi.ph");
    EXPECT_GE(ph, 2);
    EXPECT_EQ(countLabels(body, "wi.head"), ph);
    EXPECT_EQ(countLabels(body, "wi.latch"), ph);

    // Per-block shared memory: the addrspace(3) global is gone, replaced by one
    // [256 x i32] stack buffer; barriers consumed; a ctx array; single exit.
    EXPECT_EQ(countSubstr(ir, "addrspace(3) global"), 0);
    EXPECT_EQ(countSubstr(body, "alloca [256 x i32]"), 1) << "per-block buffer";
    EXPECT_EQ(countSubstr(body, "call void @__cajeta_xpu_cpu_barrier"), 0);
    EXPECT_GE(countSubstr(body, "ctx = alloca"), 1) << "t crosses a barrier";
    EXPECT_EQ(countSubstr(body, "ret void"), 1) << "single exit (wrap.end)";
}

// The SHIPPED IR (default path, post-LoopVectorize) must still carry the
// invariants that survive vectorization: barriers consumed, no addrspace(3)
// shared global, exactly one per-block stack buffer, a context array present.
TEST(XpuCpuBarrierEmitTests, shippedVectorizedWrapperKeepsInvariants) {
    std::string ir = compileToIr(kReduceSource, "test.M.reduce");
    ASSERT_FALSE(ir.empty());
    std::string body = functionBody(ir, "__cajeta_xpu_cpu_block.reduce");
    ASSERT_FALSE(body.empty()) << "no fission wrapper emitted";

    EXPECT_EQ(countSubstr(body, "call void @__cajeta_xpu_cpu_barrier"), 0);
    EXPECT_EQ(countSubstr(ir, "addrspace(3) global"), 0);
    EXPECT_EQ(countSubstr(body, "alloca [256 x i32]"), 1);
    EXPECT_GE(countSubstr(body, "ctx = alloca"), 1);
}

// Increment 7: a uniform loop body with TWO barriers splits into >=2 work-item
// loops nested inside the single outer scalar loop, and a per-work-item local
// that crosses the in-loop barrier (`x`) is widened to a context array.
TEST(XpuCpuBarrierEmitTests, multiBarrierLoopBodySplitsAndWidensLocal) {
    ScopedEnv noVec("CAJETA_XPU_CPU_NO_VECTORIZE", "1");
    std::string ir = compileToIr(kLocalCarrySource, "test.M.localcarry");
    ASSERT_FALSE(ir.empty());
    std::string body = functionBody(ir, "__cajeta_xpu_cpu_block.localcarry");
    ASSERT_FALSE(body.empty()) << "no fission wrapper emitted";

    // One uniform loop, kept as the outer scalar scaffold.
    EXPECT_EQ(countLabels(body, "for.head"), 1) << "uniform loop kept as outer";

    // The loop body splits at the in-loop barrier into >=2 work-item loops: one
    // entered from the loop header, one from after the in-loop barrier.
    int inLoop = 0;
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line))
        if (line.rfind("wi.ph", 0) == 0
            && (line.find("%for.head") != std::string::npos
                || line.find("%for.body") != std::string::npos))
            ++inLoop;
    EXPECT_GE(inLoop, 2) << "loop body must split into >=2 work-item loops";

    // The local `x` crosses the in-loop barrier ⇒ widened to a context array
    // (the memory-aware taint fixpoint — x derives from t through t's slot).
    EXPECT_EQ(countSubstr(body, "x.slot.ctx = alloca"), 1) << "x must be widened";
    EXPECT_EQ(countSubstr(body, "call void @__cajeta_xpu_cpu_barrier"), 0);
}
