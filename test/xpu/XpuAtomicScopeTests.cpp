// kernel-atomic-scope (specs/kernel-atomic-scope-spec.md): a KernelBuffer atomic is
// device-scoped on every GPU backend, and a shared array's atomic is workgroup-scoped.
// Before this, nvptx and amdgpu emitted LLVM's default system scope, so every cuda
// KernelBuffer atomic was an atom.*.sys: the bench scan's look-back ran at 1490 us on the
// 4090 against 560 us device-scoped (2026-11-19).

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"

#include <sstream>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// Every KernelBuffer atomic form at every order, plus a shared-array counter.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.MemoryOrder;
import cajeta.xpu.Shared;
public class A {
    @Kernel
    @Occupancy(maxThreads = 64)
    public static void ints(KernelBuffer<int32> b, KernelBuffer<int64> l) {
        b.atomicAdd(0, 1);
        b.atomicSub(1, 1, MemoryOrder.Relaxed);
        b.atomicMin(2, 3, MemoryOrder.Acquire);
        b.atomicMax(3, 4, MemoryOrder.Release);
        b.atomicAnd(4, 5, MemoryOrder.AcqRel);
        b.atomicOr(5, 6, MemoryOrder.SeqCst);
        b.atomicXor(6, 7);
        b.atomicExchange(7, 8, MemoryOrder.Release);
        b.atomicCompareExchange(8, 0, 9, MemoryOrder.AcqRel);
        l.atomicAdd(0, 1L);
    }
    @Kernel
    @Occupancy(maxThreads = 64)
    public static void floats(KernelBuffer<float32> f) {
        f.atomicAdd(0, 1.0f);
        f.atomicAdd(1, 2.0f, MemoryOrder.Relaxed);
    }
    @Kernel
    @Occupancy(maxThreads = 64)
    public static void sharedCounter(KernelBuffer<int32> out) {
        Shared<int32> s = shared int32[1];
        uint32 t = KernelThread.x();
        if (t == 0) {
            s[0] = 0;
        }
        Barrier.workgroup();
        s.atomicAdd(0, 1);
        Barrier.workgroup();
        if (t == 0) {
            out[0] = s[0];
        }
    }
}
)CJ";

const char* kKernels[] = {"ints", "floats", "sharedCounter"};

// The atomic instructions of a PTX listing, one per line.
std::vector<std::string> ptxAtomics(const std::string& ptx) {
    std::vector<std::string> out;
    std::istringstream in(ptx);
    for (std::string line; std::getline(in, line);)
        if (line.find("atom.") != std::string::npos || line.find("red.") != std::string::npos)
            out.push_back(line);
    return out;
}

// The atomicrmw and cmpxchg instructions of an IR listing.
std::vector<std::string> irAtomics(const std::string& ir) {
    std::vector<std::string> out;
    std::istringstream in(ir);
    for (std::string line; std::getline(in, line);)
        if (line.find("atomicrmw ") != std::string::npos || line.find("cmpxchg ") != std::string::npos)
            out.push_back(line);
    return out;
}

} // namespace

// spec §2.1, plan 1.1.1: no KernelBuffer or shared atomic on nvptx is system-scoped.
TEST(XpuAtomicScope, nvptxAtomicsAreNeverSystemScoped) {
    using namespace cajeta::xpu::probe;
    for (const char* k : kKernels) {
        Lowered l = lowerForNvptx(kSource, k, "test.A");
        ASSERT_TRUE(l.ok) << k << ": " << l.why;
        std::vector<std::string> atoms = ptxAtomics(l.ptx);
        EXPECT_FALSE(atoms.empty()) << k << " has no atomic in its PTX\n" << l.ptx;
        for (const std::string& a : atoms)
            EXPECT_EQ(a.find(".sys"), std::string::npos) << k << ": " << a;
    }
}

// The ordered forms carry the device scope by name, so the test above can't pass by
// dropping the qualifier from an acquire or a release.
TEST(XpuAtomicScope, nvptxOrderedKernelBufferAtomicsAreDeviceScoped) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(kSource, "ints", "test.A");
    ASSERT_TRUE(l.ok) << l.why;
    int ordered = 0;
    for (const std::string& a : ptxAtomics(l.ptx)) {
        if (a.find(".acquire") == std::string::npos && a.find(".release") == std::string::npos &&
            a.find(".acq_rel") == std::string::npos)
            continue;
        ++ordered;
        EXPECT_NE(a.find(".gpu"), std::string::npos) << a;
    }
    EXPECT_GE(ordered, 4) << l.ptx;
}

// plan 1.1.2: amdgpu's atomics carry syncscope("agent") on global memory and
// syncscope("workgroup") on a shared array (address space 3).
TEST(XpuAtomicScope, amdgpuAtomicsCarryAgentOrWorkgroupScope) {
    using namespace cajeta::xpu::probe;
    for (const char* arch : {"gfx1151", "gfx942"}) {
        for (const char* k : kKernels) {
            Lowered l = lowerForAmdgpu(kSource, k, "test.A", arch);
            ASSERT_TRUE(l.ok) << k << " on " << arch << ": " << l.why;
            std::vector<std::string> atoms = irAtomics(l.ir);
            EXPECT_FALSE(atoms.empty()) << k << " on " << arch << " has no atomic\n" << l.ir;
            for (const std::string& a : atoms) {
                const bool shared = a.find("addrspace(3)") != std::string::npos;
                EXPECT_NE(a.find(shared ? "syncscope(\"workgroup\")" : "syncscope(\"agent\")"),
                          std::string::npos)
                    << k << " on " << arch << ": " << a;
            }
        }
    }
}

namespace {

// plan 1.1.3: values. A counter every thread of 512 workgroups bumps, a float sum of
// ones, a shared counter, and the bench's one-pass look-back scan over 2^20.
const char* kValueSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.GroupOp;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.MemoryOrder;
import cajeta.xpu.Shared;
import cajeta.xpu.Workgroup;
public class V {
    @Kernel
    @Occupancy(maxThreads = 256)
    public static void count(KernelBuffer<int32> c, KernelBuffer<float32> f) {
        c.atomicAdd(0, 1, MemoryOrder.Relaxed);
        f.atomicAdd(0, 1.0f);
    }
    @Kernel
    @Occupancy(maxThreads = 256)
    public static void sharedCount(KernelBuffer<int32> out) {
        Shared<int32> s = shared int32[1];
        uint32 t = KernelThread.x();
        if (t == 0) {
            s[0] = 0;
        }
        Barrier.workgroup();
        s.atomicAdd(0, 1);
        Barrier.workgroup();
        if (t == 0) {
            out[Workgroup.x()] = s[0];
        }
    }
    @Kernel
    @Occupancy(maxThreads = 256)
    public static void scanReset(KernelBuffer<int32> flag, KernelBuffer<int32> counter, uint32 tiles) {
        uint32 i = KernelThread.globalIdX();
        if (i < tiles) {
            flag[i] = 0;
        }
        if (i == 0) {
            counter[0] = 0;
        }
    }
    @Kernel
    @Occupancy(maxThreads = 256)
    public static void scanLookback(KernelBuffer<int32> out, KernelBuffer<int32> x, KernelBuffer<int32> agg,
                                    KernelBuffer<int32> inc, KernelBuffer<int32> flag, KernelBuffer<int32> counter) {
        Shared<int32> bc = shared int32[2];
        uint32 t = KernelThread.x();
        if (t == 0) {
            bc[0] = counter.atomicAdd(0, 1, MemoryOrder.Relaxed);
        }
        Barrier.workgroup();
        uint32 tile = (uint32) bc[0];
        uint32 base = tile * 1024 + t * 4;
        int32 v0 = x[base];
        int32 v1 = x[base + 1];
        int32 v2 = x[base + 2];
        int32 v3 = x[base + 3];
        int32 local = (v0 + v1) + (v2 + v3);
        int32 before = Workgroup.scan(GroupOp.Add, local);
        if (t == 255) {
            int32 a = before + local;
            agg.atomicExchange(tile, a, MemoryOrder.Relaxed);
            flag.atomicExchange(tile, 1, MemoryOrder.Release);
            bc[1] = a;
        }
        Barrier.workgroup();
        if (t == 0) {
            int32 run = 0;
            if (tile > 0) {
                uint32 p = tile - 1;
                uint32 spins = 0;
                boolean done = false;
                while (!done) {
                    int32 fl = flag.atomicAdd(p, 0, MemoryOrder.Acquire);
                    if (fl == 2) {
                        run = run + inc.atomicAdd(p, 0, MemoryOrder.Relaxed);
                        done = true;
                    } else if (fl == 1) {
                        run = run + agg.atomicAdd(p, 0, MemoryOrder.Relaxed);
                        if (p == 0) {
                            done = true;
                        } else {
                            p = p - 1;
                        }
                    } else {
                        spins = spins + 1;
                        if (spins > 10000000) {
                            done = true;
                        }
                    }
                }
            }
            inc.atomicExchange(tile, run + bc[1], MemoryOrder.Relaxed);
            flag.atomicExchange(tile, 2, MemoryOrder.Release);
            bc[0] = run;
        }
        Barrier.workgroup();
        int32 r = bc[0] + before;
        r = r + v0; out[base] = r;
        r = r + v1; out[base + 1] = r;
        r = r + v2; out[base + 2] = r;
        r = r + v3; out[base + 3] = r;
    }

    // 1 when 512 x 256 relaxed int adds and float adds of one both reach 131072.
    public static int32 counters() {
        KernelBuffer<int32> c = heap KernelBuffer<int32>(1);
        KernelBuffer<float32> f = heap KernelBuffer<float32>(1);
        int32[] hc = heap int32[1];
        float32[] hf = heap float32[1];
        c.upload(hc);
        f.upload(hf);
        KernelStream s #= KernelStream.current();
        count.launch(s, grid: [512], block: [256])(c, f);
        s.sync();
        c.download(hc);
        f.download(hf);
        if (hc[0] != 131072) { return -hc[0]; }
        if (hf[0] != 131072.0f) { return -2; }
        return 1;
    }

    // 1 when every one of 64 workgroups counts its 256 threads in shared memory.
    public static int32 sharedCounters() {
        KernelBuffer<int32> out = heap KernelBuffer<int32>(64);
        int32[] h = heap int32[64];
        KernelStream s #= KernelStream.current();
        sharedCount.launch(s, grid: [64], block: [256])(out);
        s.sync();
        out.download(h);
        int32 i = 0;
        while (i < 64) {
            if (h[i] != 256) { return -i - 1; }
            i = i + 1;
        }
        return 1;
    }

    // 1 when the look-back scan of 2^20 matches the host's; else minus the first
    // wrong index, less one.
    public static int32 scan() {
        int32 n = 1048576;
        int32[] hx = heap int32[n];
        int32[] ref = heap int32[n];
        int32[] h = heap int32[n];
        int32 run = 0;
        int32 i = 0;
        while (i < n) { int32 v = (i % 5) - 2; hx[i] = v; run = run + v; ref[i] = run; i = i + 1; }
        KernelBuffer<int32> x = heap KernelBuffer<int32>((uint64) n);
        x.upload(hx);
        KernelBuffer<int32> out = heap KernelBuffer<int32>((uint64) n);
        uint32 tiles = (uint32) n / 1024;
        KernelBuffer<int32> agg = heap KernelBuffer<int32>((uint64) tiles);
        KernelBuffer<int32> inc = heap KernelBuffer<int32>((uint64) tiles);
        KernelBuffer<int32> flag = heap KernelBuffer<int32>((uint64) tiles);
        KernelBuffer<int32> counter = heap KernelBuffer<int32>(1);
        KernelStream s #= KernelStream.current();
        scanReset.launch(s, grid: [(tiles + 255) / 256], block: [256])(flag, counter, tiles);
        scanLookback.launch(s, grid: [tiles], block: [256])(out, x, agg, inc, flag, counter);
        s.sync();
        out.download(h);
        i = 0;
        while (i < n) {
            if (h[i] != ref[i]) { return -i - 1; }
            i = i + 1;
        }
        return 1;
    }
}
)CJ";

void valuesHold(cajeta::xpu::Backend be, const char* name, bool withScan) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kValueSource, "test.V", o);
    ASSERT_TRUE(jit) << name << ": compile failed";
    auto counters = jit->lookup<int32_t (*)()>("counters");
    auto shared = jit->lookup<int32_t (*)()>("sharedCounters");
    ASSERT_TRUE(counters && shared) << name;
    EXPECT_EQ(1, counters()) << name << " counters";
    EXPECT_EQ(1, shared()) << name << " shared counters";
    if (withScan) {
        auto scan = jit->lookup<int32_t (*)()>("scan");
        ASSERT_TRUE(scan) << name;
        EXPECT_EQ(1, scan()) << name << " scan";
    }
}

} // namespace

// The cpu pool runs a cross-workgroup spin only when every block is live at once,
// which it does not promise, so the scan is left to the GPU backends.
TEST(XpuAtomicScope, valuesHoldOnCpu) {
    valuesHold(cajeta::xpu::Backend::Cpu, "cpu", false);
}

TEST(XpuAtomicScopeDeviceTests, valuesHoldOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    valuesHold(cajeta::xpu::Backend::Nvptx, "nvptx", true);
}

TEST(XpuAtomicScopeDeviceTests, valuesHoldOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    valuesHold(cajeta::xpu::Backend::Amdgpu, "amdgpu", true);
}

TEST(XpuAtomicScopeDeviceTests, valuesHoldOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    valuesHold(cajeta::xpu::Backend::Spirv, "vulkan", true);
}

// ---- Unit 2 (spec §2.2): a kernel names the scope --------------------------------

namespace {

// One kernel per scope, each with an ordered int add, a float add and a
// compare-exchange at that scope, so the scope qualifier is printed on every form.
std::string scopedSource(bool constant) {
    std::string s = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.MemoryOrder;
import cajeta.xpu.MemoryScope;
public class S {
)CJ";
    for (const char* sc : {"Workgroup", "Device", "System"}) {
        s += std::string("    @Kernel\n    @Occupancy(maxThreads = 64)\n    public static void at") + sc +
             "(KernelBuffer<int32> b, KernelBuffer<float32> f) {\n"
             "        b.atomicAdd(0, 1, MemoryOrder.AcqRel, MemoryScope." + sc + ");\n"
             "        f.atomicAdd(0, 1.0f, MemoryOrder.AcqRel, MemoryScope." + sc + ");\n"
             "        b.atomicCompareExchange(1, 0, 1, MemoryOrder.AcqRel, MemoryScope." + sc + ");\n"
             "    }\n";
    }
    if (!constant)
        s += "    @Kernel\n    @Occupancy(maxThreads = 64)\n"
             "    public static void notConstant(KernelBuffer<int32> b) {\n"
             "        MemoryScope sc = MemoryScope.System;\n"
             "        b.atomicAdd(0, 1, MemoryOrder.Relaxed, sc);\n"
             "    }\n";
    return s + "}\n";
}

} // namespace

// plan 2.1.1: System is .sys, Workgroup .cta and Device .gpu on nvptx.
TEST(XpuAtomicScope, nvptxHonoursANamedScope) {
    using namespace cajeta::xpu::probe;
    const std::string src = scopedSource(true);
    const struct { const char* kernel; const char* want; } wants[] = {
        {"atWorkgroup", ".cta"}, {"atDevice", ".gpu"}, {"atSystem", ".sys"}};
    for (const auto& w : wants) {
        Lowered l = lowerForNvptx(src, w.kernel, "test.S");
        ASSERT_TRUE(l.ok) << w.kernel << ": " << l.why;
        std::vector<std::string> atoms = ptxAtomics(l.ptx);
        EXPECT_GE(atoms.size(), 3u) << w.kernel << "\n" << l.ptx;
        for (const std::string& a : atoms)
            EXPECT_NE(a.find(w.want), std::string::npos) << w.kernel << " wants " << w.want << ": " << a;
    }
}

// plan 2.1.1: amdgpu's sync scopes, System being LLVM's default (no syncscope).
TEST(XpuAtomicScope, amdgpuHonoursANamedScope) {
    using namespace cajeta::xpu::probe;
    const std::string src = scopedSource(true);
    for (const char* arch : {"gfx1151", "gfx942"}) {
        const struct { const char* kernel; const char* want; } wants[] = {
            {"atWorkgroup", "syncscope(\"workgroup\")"}, {"atDevice", "syncscope(\"agent\")"},
            {"atSystem", nullptr}};
        for (const auto& w : wants) {
            Lowered l = lowerForAmdgpu(src, w.kernel, "test.S", arch);
            ASSERT_TRUE(l.ok) << w.kernel << " on " << arch << ": " << l.why;
            std::vector<std::string> atoms = irAtomics(l.ir);
            EXPECT_GE(atoms.size(), 2u) << w.kernel << " on " << arch << "\n" << l.ir;
            for (const std::string& a : atoms) {
                if (w.want)
                    EXPECT_NE(a.find(w.want), std::string::npos) << w.kernel << " on " << arch << ": " << a;
                else
                    EXPECT_EQ(a.find("syncscope("), std::string::npos) << w.kernel << " on " << arch << ": " << a;
            }
        }
    }
}

// plan 2.1.3: a scope that isn't a compile-time constant is refused, naming the method.
TEST(XpuAtomicScope, aScopeThatIsNotAConstantIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(scopedSource(false), "notConstant", "test.S");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("atomicAdd"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("MemoryScope"), std::string::npos) << l.why;
}

namespace {

// plan 2.1.2: the kernel publishes a pinned flag at system scope and the host sees
// it while the launch is still running. (The reverse, a kernel spinning on a host
// write to pinned memory, never saw it on Phoenix under WSL2, 2026-11-20.)
// Also the scoped kernels' values on any backend.
const char* kHandshakeSource = R"CJ(
package test;
import cajeta.time.Clock;
import cajeta.xpu.Event;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.MemoryKind;
import cajeta.xpu.MemoryOrder;
import cajeta.xpu.MemoryScope;
public class H {
    // Publishes the flag at system scope, then stays running about a second of
    // its own clock, so the host can see the flag while the launch is live.
    @Kernel
    @Occupancy(maxThreads = 32)
    public static void signal(KernelBuffer<int32> ready, KernelBuffer<int64> out) {
        if (KernelThread.globalIdX() == 0) {
            ready.atomicExchange(0, 1, MemoryOrder.Release, MemoryScope.System);
            uint64 t0 = KernelThread.clock();
            uint64 now = t0;
            while (now - t0 < 2000000000L) {
                now = KernelThread.clock();
            }
            out[0] = (int64) (now - t0);
        }
    }
    @Kernel
    @Occupancy(maxThreads = 256)
    public static void scoped(KernelBuffer<int32> b, KernelBuffer<float32> f) {
        b.atomicAdd(0, 1, MemoryOrder.Relaxed, MemoryScope.Device);
        b.atomicAdd(1, 1, MemoryOrder.AcqRel, MemoryScope.System);
        b.atomicAdd(2, 1, MemoryOrder.Relaxed, MemoryScope.Workgroup);
        f.atomicAdd(0, 1.0f, MemoryOrder.Relaxed, MemoryScope.System);
    }

    // 1 when the host saw the kernel's flag while the launch's event still read
    // not-done; -1 when the flag never showed before the launch ended, -2 when it
    // only showed after.
    public static int32 handshake() {
        // allocate(kind), not a (count, kind) constructor: none exists, and
        // `KernelBuffer(1, MemoryKind.Pinned)` binds the (handle, count) form.
        KernelBuffer<int32> ready = heap KernelBuffer<int32>(0, 1);
        ready.allocate(MemoryKind.Pinned);
        KernelBuffer<int64> out = heap KernelBuffer<int64>(1);
        int32[] h = heap int32[1];
        ready.hostStore(h);
        KernelStream s #= KernelStream.current();
        signal.launch(s, grid: [1], block: [32])(ready, out);
        Event e #= Event.create();
        e.recordOn(s);
        int64 deadline = Clock.nanoTime() + 20000000000L;
        int32 result = -1;
        while (result == -1 && Clock.nanoTime() < deadline) {
            boolean done = e.query();
            ready.hostLoad(h);
            if (h[0] == 1) {
                result = done ? -2 : 1;
            } else if (done) {
                deadline = 0L;
            }
        }
        s.sync();
        return result;
    }

    // 1 when 64 x 256 adds at each scope all reach 16384.
    public static int32 scopedValues() {
        KernelBuffer<int32> b = heap KernelBuffer<int32>(3);
        KernelBuffer<float32> f = heap KernelBuffer<float32>(1);
        int32[] hb = heap int32[3];
        float32[] hf = heap float32[1];
        b.upload(hb);
        f.upload(hf);
        KernelStream s #= KernelStream.current();
        scoped.launch(s, grid: [64], block: [256])(b, f);
        s.sync();
        b.download(hb);
        f.download(hf);
        if (hb[0] != 16384 || hb[1] != 16384) { return -1; }
        if (hf[0] != 16384.0f) { return -3; }
        return 1;
    }
}
)CJ";

int32_t runH(cajeta::xpu::Backend be, const char* fn) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kHandshakeSource, "test.H", o);
    if (!jit) return -100;
    auto f = jit->lookup<int32_t (*)()>(fn);
    return f ? f() : -101;
}

} // namespace

// The workgroup-scoped counter (b[2]) is not checked across workgroups: 64 groups
// each own their adds, and the sum is only promised at device scope or wider.
TEST(XpuAtomicScope, scopedAtomicsKeepTheirValuesOnCpu) {
    EXPECT_EQ(1, runH(cajeta::xpu::Backend::Cpu, "scopedValues"));
}

TEST(XpuAtomicScopeDeviceTests, scopedAtomicsKeepTheirValuesOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    EXPECT_EQ(1, runH(cajeta::xpu::Backend::Nvptx, "scopedValues"));
}

TEST(XpuAtomicScopeDeviceTests, scopedAtomicsKeepTheirValuesOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    EXPECT_EQ(1, runH(cajeta::xpu::Backend::Amdgpu, "scopedValues"));
}

// Vulkan has no shader scope wider than Device that drivers accept, so System
// lowers to Device there (spec §2.2) and the kernel still runs.
TEST(XpuAtomicScopeDeviceTests, scopedAtomicsKeepTheirValuesOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    EXPECT_EQ(1, runH(cajeta::xpu::Backend::Spirv, "scopedValues"));
}

TEST(XpuAtomicScopeDeviceTests, aSystemScopedFlagReachesTheHostMidLaunchOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    EXPECT_EQ(1, runH(cajeta::xpu::Backend::Nvptx, "handshake"));
}
