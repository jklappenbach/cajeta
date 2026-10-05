//
// The kernel reference interpreter (xpu-kernel-independence plan Unit 0, spec
// §2.2 and §7.2).
//
// The interpreter runs a kernel's source on the host as the expected answer.
// Each test here runs one kernel twice, once on the cpu backend and once in
// the interpreter, and compares: integers bit for bit, floats within a stated
// ulp count. The last tests check the instrument itself: a construct the
// interpreter cannot define is refused before anything runs, and a lowering
// broken on purpose is caught.
//
#include "gtest/gtest.h"

#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "../PortableEnv.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/reference/KernelInterpreter.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ref = cajeta::xpu::reference;
using cajeta_test::CpuKernel;
using cajeta_test::findKernel;

namespace {

const char* kImports = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.Bits;
import cajeta.xpu.BlockPadded;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.Group;
import cajeta.xpu.GroupOp;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Wave;
import cajeta.xpu.Workgroup;
)CJ";

std::string source(const std::string& body) {
    return std::string(kImports) + "public class M {\n" + body + "\n}\n";
}

// One compiled kernel, run on both sides over copies of the same inputs.
struct Pair {
    cajeta::Compiler compiler;
    cajeta::CajetaModulePtr module;
    cajeta::MethodPtr kernel;
    std::string failure;

    Pair(const std::string& body, const std::string& name) {
        module = cajeta::xpu::probe::compileForInspection(compiler, source(body));
        if (!module) { failure = "did not compile"; return; }
        kernel = findKernel(module, "test.M", name);
        if (!kernel) failure = "no kernel " + name;
    }
};

// A launch shape and its argument list, as buffers of bytes so one list
// serves both sides.
struct Buf {
    std::vector<uint8_t> bytes;
    uint64_t elements = 0;
    void* ptr = nullptr;       // what the cpu argv slot points at
};

template <typename T>
Buf bufOf(const std::vector<T>& v) {
    Buf b;
    b.bytes.resize(v.size() * sizeof(T));
    if (!v.empty()) std::memcpy(b.bytes.data(), v.data(), b.bytes.size());
    b.elements = v.size();
    return b;
}

template <typename T>
std::vector<T> as(const Buf& b) {
    std::vector<T> out(b.bytes.size() / sizeof(T));
    if (!out.empty()) std::memcpy(out.data(), b.bytes.data(), b.bytes.size());
    return out;
}

// An argument in parameter order: a buffer, or a scalar's bits.
struct In {
    bool isBuffer = false;
    Buf buf;
    uint64_t bits = 0;
};

template <typename T> In buffer(const std::vector<T>& v) { In i; i.isBuffer = true; i.buf = bufOf(v); return i; }
In u32(uint32_t v) { In i; i.bits = v; return i; }
In f32(float f) { In i; uint32_t b; std::memcpy(&b, &f, 4); i.bits = b; return i; }

struct Shape {
    uint32_t grid[3] = {1, 1, 1};
    uint32_t block[3] = {1, 1, 1};
};

Shape shape(uint32_t g, uint32_t b) { Shape s; s.grid[0] = g; s.block[0] = b; return s; }

// Run on the cpu backend; buffers are updated in place.
std::string runCpu(const cajeta::MethodPtr& k, std::vector<In>& ins, const Shape& s) {
    std::string failure;
    auto cpu = CpuKernel::load(k, failure);
    if (!cpu) return failure;
    std::vector<void*> argv;
    std::vector<uint64_t> scalars(ins.size());
    for (size_t i = 0; i < ins.size(); ++i) {
        if (ins[i].isBuffer) {
            ins[i].buf.ptr = ins[i].buf.bytes.data();
            argv.push_back(&ins[i].buf.ptr);
        } else {
            scalars[i] = ins[i].bits;
            argv.push_back(&scalars[i]);
        }
    }
    if (!cpu->launch(argv.data(), s.grid, s.block)) return "the cpu launch was refused";
    return "";
}

// Run in the interpreter; buffers are updated in place.
std::string runRef(const cajeta::MethodPtr& k, std::vector<In>& ins, const Shape& s,
                   uint32_t wave = 0) {
    std::vector<ref::Arg> args;
    for (auto& in : ins)
        args.push_back(in.isBuffer ? ref::Arg::buffer(in.buf.bytes.data(), in.buf.elements)
                                   : ref::Arg::scalar(in.bits));
    ref::Launch l;
    for (int d = 0; d < 3; ++d) { l.grid[d] = s.grid[d]; l.block[d] = s.block[d]; }
    l.waveWidth = wave;
    try {
        ref::run(k, args, l);
    } catch (cajeta::Exception& e) {
        return e.getErrorId() + ": " + e.getMessage();
    }
    return "";
}

// "" when every element agrees, else the first disagreement, naming the
// kernel, the backend and the element.
template <typename T>
std::string disagreement(const std::string& kernel, const std::string& backend,
                         const std::vector<T>& expected, const std::vector<T>& got,
                         uint64_t ulps = 0) {
    if (expected.size() != got.size()) return kernel + " on " + backend + ": size differs";
    for (size_t i = 0; i < got.size(); ++i) {
        bool same;
        if constexpr (std::is_floating_point_v<T>)
            same = ref::ulpDistance(expected[i], got[i]) <= ulps;
        else
            same = expected[i] == got[i];
        if (!same) {
            std::ostringstream os;
            os << kernel << " on " << backend << " disagrees with the reference at element "
               << i << ": expected " << +expected[i] << ", got " << +got[i];
            return os.str();
        }
    }
    return "";
}

// Run both sides over copies of `ins`, and compare output buffer `out` as T.
template <typename T>
std::string compareOn(Pair& p, const std::vector<In>& ins, size_t out, const Shape& s,
                      uint64_t ulps = 0, uint32_t wave = 0) {
    if (!p.failure.empty()) return p.failure;
    std::vector<In> cpu = ins, ref = ins;
    std::string why = runCpu(p.kernel, cpu, s);
    if (!why.empty()) return "cpu: " + why;
    why = runRef(p.kernel, ref, s, wave);
    if (!why.empty()) return "reference: " + why;
    return disagreement(p.kernel->getName(), "cpu", as<T>(ref[out].buf), as<T>(cpu[out].buf),
                        ulps);
}

std::vector<float> ramp(size_t n, float scale, float bias) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = scale * (float) i + bias;
    return v;
}

} // namespace

// ---- 4.0.1.1: three kernel shapes match the cpu backend ------------------

TEST(XpuReferenceInterpreter, aScalarKernelMatchesTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    public static void refSaxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,
                                float32 a, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            y[i] = a * x[i] + y[i];
        }
    }
)CJ", "refSaxpy");
    const uint32_t N = 256;
    std::vector<In> ins = {buffer(ramp(N, 0.37f, 1.0f)), buffer(ramp(N, -1.13f, 0.5f)),
                           f32(2.71828f), u32(200)};
    // Bound: 0 ulp. Two roundings on both sides; nothing contracts a*x+y.
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(4, 64)), "");
}

TEST(XpuReferenceInterpreter, aSharedMemoryKernelWithABarrierMatchesTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    public static void refRotate(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> tile = shared int32[64];
        uint32 t = KernelThread.x();
        uint32 g = KernelThread.globalIdX();
        tile[t] = in[g] * 3 - (int32) Workgroup.x();
        Barrier.workgroup();
        out[g] = tile[63 - t] - tile[(t + 1) % 64];
    }
)CJ", "refRotate");
    std::vector<int32_t> in(256);
    for (int i = 0; i < 256; ++i) in[i] = (i * 7919) % 1000 - 500;
    std::vector<In> ins = {buffer(std::vector<int32_t>(256)), buffer(in)};
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(4, 64)), "");
}

TEST(XpuReferenceInterpreter, aWaveReduceKernelMatchesTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refWave(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 g = KernelThread.globalIdX();
        uint32 v = in[g];
        uint32 s = Wave.reduceSum(v);
        uint32 m = Wave.reduceMax(v);
        uint32 x = Wave.reduceXor(v);
        out[g] = s * 7 + m - x + Wave.laneId() * 100000;
    }
)CJ", "refWave");
    std::vector<uint32_t> in(128);
    for (uint32_t i = 0; i < 128; ++i) in[i] = (i * 2654435761) >> 7;
    std::vector<In> ins = {buffer(std::vector<uint32_t>(128)), buffer(in)};
    EXPECT_EQ(compareOn<uint32_t>(p, ins, 0, shape(2, 64)), "");
}

// ---- 4.0.1.2: integers bit for bit, floats in the declared precision ------

TEST(XpuReferenceInterpreter, integerKernelsMatchBitForBit) {
    Pair p(R"CJ(
    @Kernel
    public static void refInts(KernelBuffer<int32> out, KernelBuffer<int32> a,
                               KernelBuffer<uint32> b) {
        uint32 i = KernelThread.globalIdX();
        int32 x = a[i];
        uint32 u = b[i];
        int64 wide = (int64) x * 1000003L;
        int32 q = x / 7;
        int32 r = x % 7;
        uint32 uq = u / 7;
        int32 sh = x >> 3;
        uint32 ush = u >> 3;
        int32 mix = x * 31 + (int32) (u & 255);
        int32 c = 0;
        if (x < 0) { c = 1; }
        if (u > 3000000000) { c = c + 2; }
        out[i * 6] = q;
        out[i * 6 + 1] = r;
        out[i * 6 + 2] = (int32) uq ^ sh;
        out[i * 6 + 3] = (int32) ush + mix;
        out[i * 6 + 4] = (int32) (wide >> 17);
        out[i * 6 + 5] = c;
    }
)CJ", "refInts");
    const uint32_t N = 128;
    std::vector<int32_t> a(N);
    std::vector<uint32_t> b(N);
    for (uint32_t i = 0; i < N; ++i) {
        a[i] = (int32_t) (i * 2246822519);       // both signs, both magnitudes
        b[i] = i * 3266489917;
    }
    std::vector<In> ins = {buffer(std::vector<int32_t>(N * 6)), buffer(a), buffer(b)};
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(2, 64)), "");
}

TEST(XpuReferenceInterpreter, floatsAreEvaluatedInTheDeclaredPrecision) {
    Pair p(R"CJ(
    @Kernel
    public static void refPrecision(KernelBuffer<float32> out, KernelBuffer<float32> x,
                                    KernelBuffer<float32> y) {
        uint32 i = KernelThread.globalIdX();
        float32 t = x[i] * y[i];
        out[i] = t / 3.0f + x[i];
    }
)CJ", "refPrecision");
    ASSERT_EQ(p.failure, "");
    const uint32_t N = 64;
    std::vector<float> x = ramp(N, 0.1f, 1.0f / 3.0f), y = ramp(N, -0.7f, 3.3f);
    std::vector<In> ins = {buffer(std::vector<float>(N)), buffer(x), buffer(y)};

    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(1, N)), "");
    std::vector<float> got = as<float>(r[0].buf);
    size_t differsFromDouble = 0;
    for (uint32_t i = 0; i < N; ++i) {
        float t = x[i] * y[i];
        float single = t / 3.0f + x[i];
        float viaDouble = (float) ((double) x[i] * (double) y[i] / 3.0 + (double) x[i]);
        EXPECT_EQ(ref::ulpDistance(got[i], single), 0) << "element " << i;
        if (single != viaDouble) ++differsFromDouble;
    }
    EXPECT_GT(differsFromDouble, 0) << "the inputs do not tell f32 from f64 evaluation";

    // Bound: 0 ulp against the cpu backend, which rounds at the same points.
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(1, N)), "");
}

TEST(XpuReferenceInterpreter, ulpDistanceCountsRepresentableSteps) {
    EXPECT_EQ(ref::ulpDistance(1.0f, 1.0f), 0);
    EXPECT_EQ(ref::ulpDistance(0.0f, -0.0f), 0);
    EXPECT_EQ(ref::ulpDistance(1.0f, std::nextafter(1.0f, 2.0f)), 1);
    EXPECT_EQ(ref::ulpDistance(-std::nextafter(0.0f, 1.0f), std::nextafter(0.0f, 1.0f)), 2);
    EXPECT_EQ(ref::ulpDistance(1.0, std::nextafter(std::nextafter(1.0, 2.0), 2.0)), 2);
    EXPECT_EQ(ref::ulpDistance(std::nanf(""), 1.0f), UINT64_MAX);
}

// ---- 4.0.1.3: built-ins have reference semantics, or are refused by name --

TEST(XpuReferenceInterpreter, cooperativeMatrixFragmentsMatchTheCpuBackend) {
    // C[16x16] = A[16x32] . B[32x16] over two K tiles, B read column-major.
    Pair p(R"CJ(
    @Kernel
    public static void refTile(KernelBuffer<float32> c, KernelBuffer<float32> a,
                               KernelBuffer<float32> b) {
        CooperativeMatrix<float32,16,16,0> ta;
        CooperativeMatrix<float32,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        acc.splat(1.0f);
        for (uint32 kt = 0; kt < 2; kt = kt + 1) {
            ta.load(a, kt * 16, 0, 32);
            tb.load(b, kt * 16, 1, 32);
            acc.mma(ta, tb);
        }
        acc.store(c, 0, 0, 16);
    }
)CJ", "refTile");
    std::vector<float> a(16 * 32), b(32 * 16);
    for (int i = 0; i < 16 * 32; ++i) { a[i] = (float) ((i * 7) % 11 - 5); b[i] = (float) ((i * 5) % 9 - 4); }
    std::vector<In> ins = {buffer(std::vector<float>(256)), buffer(a), buffer(b)};
    // Bound: 0 ulp. Small integers, so every partial sum is exact in f32.
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(1, 32)), "");
}

TEST(XpuReferenceInterpreter, dotAccumMatchesTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    public static void refDotAccum(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                                   KernelBuffer<int8> a, KernelBuffer<int32> seed) {
        uint32 g = KernelThread.globalIdX();
        int64 wo = (int64) g * 16L;
        int64 so = (int64) g * 4L;
        Vector<uint8,16> wv = w.vload<16>(wo);
        Vector<int8,16> av = a.vload<16>(wo);
        Vector<int32,4> acc = seed.vload<4>(so);
        out.vstore(so, wv.dotAccum(av, acc));
    }
)CJ", "refDotAccum");
    const int G = 32;
    std::vector<uint8_t> w(G * 16);
    std::vector<int8_t> a(G * 16);
    std::vector<int32_t> seed(G * 4);
    for (int i = 0; i < G * 16; ++i) { w[i] = (uint8_t) (i * 37 + 200); a[i] = (int8_t) (i * 53 - 90); }
    for (int i = 0; i < G * 4; ++i) seed[i] = i * 1000 - 50000;
    std::vector<In> ins = {buffer(std::vector<int32_t>(G * 4)), buffer(w), buffer(a), buffer(seed)};
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(1, G)), "");

    // And the reference itself is unsigned weights times SIGNED activations.
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(1, G)), "");
    std::vector<int32_t> got = as<int32_t>(r[0].buf);
    for (int j = 0; j < G * 4; ++j) {
        int32_t want = seed[j];
        for (int k = 0; k < 4; ++k) want += (int32_t) w[j * 4 + k] * (int32_t) a[j * 4 + k];
        ASSERT_EQ(got[j], want) << "lane " << j;
    }
}

// KernelThread.clock() reads a free-running counter, so it has no reference
// value by nature and stays refused; the tests below use it for that.
TEST(XpuReferenceInterpreter, anOperationWithNoReferenceSemanticsIsRefusedByName) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refRefused(KernelBuffer<float32> out) {
        uint32 g = KernelThread.globalIdX();
        out[g] = 7.0f;
        out[g] = (float32) KernelThread.clock();
    }
)CJ", "refRefused");
    ASSERT_EQ(p.failure, "");
    auto why = ref::refusals(p.kernel);
    ASSERT_EQ(why.size(), 1);
    EXPECT_NE(why[0].find("KernelThread.clock"), std::string::npos) << why[0];
    EXPECT_NE(why[0].find("line"), std::string::npos) << why[0];

    std::vector<In> ins = {buffer(std::vector<float>(32, -1.0f))};
    std::string run = runRef(p.kernel, ins, shape(1, 32));
    EXPECT_EQ(run.rfind("XPU-REF01", 0), 0) << run;
    EXPECT_NE(run.find("KernelThread.clock"), std::string::npos) << run;
    for (float v : as<float>(ins[0].buf))
        ASSERT_EQ(v, -1.0f) << "a refused kernel ran partially";
}

TEST(XpuReferenceInterpreter, undefinedBehaviourFailsTheRunByName) {
    Pair p(R"CJ(
    @Kernel
    public static void refOutOfBounds(KernelBuffer<int32> out) {
        uint32 g = KernelThread.globalIdX();
        out[g + 1] = 1;
    }
    @Kernel
    public static void refHalfBarrier(KernelBuffer<int32> out) {
        uint32 t = KernelThread.x();
        if (t < 16) { return; }
        Barrier.workgroup();
        out[t] = 1;
    }
    @Kernel
    public static void refUnwrittenShared(KernelBuffer<int32> out) {
        Shared<int32> tile = shared int32[32];
        uint32 t = KernelThread.x();
        if (t != 5) { tile[t] = 1; }
        Barrier.workgroup();
        out[t] = tile[(t + 1) % 32];
    }
)CJ", "refOutOfBounds");
    ASSERT_EQ(p.failure, "");
    std::vector<In> ins = {buffer(std::vector<int32_t>(32))};
    std::string why = runRef(p.kernel, ins, shape(1, 32));
    EXPECT_EQ(why.rfind("XPU-REF02", 0), 0) << why;
    EXPECT_NE(why.find("out of bounds"), std::string::npos) << why;

    auto half = findKernel(p.module, "test.M", "refHalfBarrier");
    ins = {buffer(std::vector<int32_t>(32))};
    why = runRef(half, ins, shape(1, 32));
    EXPECT_EQ(why.rfind("XPU-REF02", 0), 0) << why;
    EXPECT_NE(why.find("Barrier.workgroup"), std::string::npos) << why;

    auto unwritten = findKernel(p.module, "test.M", "refUnwrittenShared");
    ins = {buffer(std::vector<int32_t>(32))};
    why = runRef(unwritten, ins, shape(1, 32));
    EXPECT_EQ(why.rfind("XPU-REF02", 0), 0) << why;
    EXPECT_NE(why.find("tile[5]"), std::string::npos) << why;
}

// ---- 4.0.1.4: a lowering broken on purpose is caught where it is broken ---

// CAJETA_XPU_FAULT=dot-activation-sign puts back the single-flag dot seam that
// cajeta 3ba58bda fixed: with an unsigned receiver it zero-extends the
// activations too. The interpreter must catch it, naming kernel and backend,
// and must agree again once the fault is gone.
TEST(XpuReferenceInterpreter, aFlippedActivationSignIsCaughtOnTheBackendThatHasIt) {
    const char* body = R"CJ(
    @Kernel
    public static void refDotFault(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                                   KernelBuffer<int8> a, KernelBuffer<int32> zero) {
        uint32 g = KernelThread.globalIdX();
        Vector<uint8,16> wv = w.vload<16>((int64) g * 16L);
        Vector<int8,16> av = a.vload<16>((int64) g * 16L);
        Vector<int32,4> acc = zero.vload<4>((int64) g * 4L);
        out.vstore((int64) g * 4L, wv.dotAccum(av, acc));
    }
)CJ";
    const int G = 8;
    std::vector<uint8_t> w(G * 16);
    std::vector<int8_t> a(G * 16);
    for (int i = 0; i < G * 16; ++i) { w[i] = (uint8_t) (i * 29 + 3); a[i] = (int8_t) (-(i % 100)); }
    std::vector<In> ins = {buffer(std::vector<int32_t>(G * 4)), buffer(w), buffer(a),
                           buffer(std::vector<int32_t>(G * 4))};

    setenv("CAJETA_XPU_FAULT", "dot-activation-sign", 1);
    std::string faulted;
    {
        Pair p(body, "refDotFault");
        faulted = compareOn<int32_t>(p, ins, 0, shape(1, G));
    }
    unsetenv("CAJETA_XPU_FAULT");
    EXPECT_NE(faulted.find("refDotFault on cpu disagrees with the reference"),
              std::string::npos) << "the faulted lowering was not caught: '" << faulted << "'";

    Pair clean(body, "refDotFault");
    EXPECT_EQ(compareOn<int32_t>(clean, ins, 0, shape(1, G)), "");
}

// ---- Unit 1: @Device helpers, and the survey lever ------------------------

// A helper in the kernel's class, one in another class, and one that takes a
// buffer: each is a call with its own frame, arguments converted to the
// helper's parameter types and the result to its return type.
TEST(XpuReferenceInterpreter, deviceHelperCallsMatchTheCpuBackend) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, std::string(kImports) + R"CJ(
public class Ops {
    @Device
    public static int32 mix(int32 a, uint32 b) {
        int32 t = a * 7;
        if (t < 0) { return t - (int32) (b & 15); }
        return t + (int32) (b >> 3);
    }
}
public class M {
    @Device
    public static float32 scaleOf(KernelBuffer<float32> s, uint32 i) {
        return s[i / 4] * 0.5f;
    }
    @Device
    public static int64 widen(int32 v) { return (int64) v * 3000000L; }
    @Kernel
    public static void refHelpers(KernelBuffer<int32> out, KernelBuffer<float32> fout,
                                  KernelBuffer<int32> a, KernelBuffer<float32> s) {
        uint32 g = KernelThread.globalIdX();
        int32 m = Ops.mix(a[g], g * 2654435761);
        out[g] = m ^ (int32) (widen(m) >> 20);
        fout[g] = scaleOf(s, g) + (float32) m;
    }
}
)CJ");
    ASSERT_NE(module, nullptr);
    auto k = findKernel(module, "test.M", "refHelpers");
    ASSERT_NE(k, nullptr);
    EXPECT_TRUE(ref::refusals(k).empty()) << ref::refusals(k)[0];
    const uint32_t N = 64;
    std::vector<int32_t> a(N);
    for (uint32_t i = 0; i < N; ++i) a[i] = (int32_t) (i * 2246822519u);
    std::vector<In> ins = {buffer(std::vector<int32_t>(N)), buffer(std::vector<float>(N)),
                           buffer(a), buffer(ramp(N / 4, 1.25f, -3.0f))};
    std::vector<In> cpu = ins, r = ins;
    ASSERT_EQ(runCpu(k, cpu, shape(1, N)), "");
    ASSERT_EQ(runRef(k, r, shape(1, N)), "");
    EXPECT_EQ(disagreement("refHelpers", "cpu", as<int32_t>(r[0].buf), as<int32_t>(cpu[0].buf)), "");
    EXPECT_EQ(disagreement("refHelpers", "cpu", as<float>(r[1].buf), as<float>(cpu[1].buf)), "");
}

// A refusal inside a helper names the helper, so the reader goes straight to it.
TEST(XpuReferenceInterpreter, aRefusalInsideAHelperNamesTheHelper) {
    Pair p(R"CJ(
    @Device
    public static uint32 spread(uint32 v) {
        if (KernelThread.clock() > 0L) { return v; }
        return 0;
    }
    @Kernel
    @Wave(width = 32)
    public static void refInHelper(KernelBuffer<uint32> out) {
        out[KernelThread.globalIdX()] = spread(3);
    }
)CJ", "refInHelper");
    ASSERT_EQ(p.failure, "");
    auto why = ref::refusals(p.kernel);
    ASSERT_FALSE(why.empty());
    EXPECT_NE(why[0].find("in test.M.spread"), std::string::npos) << why[0];
}

// CAJETA_XPU_REF_SURVEY=<file>: the compiler writes one line per kernel it
// lowers, saying whether the interpreter can run it.
TEST(XpuReferenceInterpreter, theSurveyLeverWritesOneLinePerKernel) {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("ref-survey-" + std::to_string(cajeta_getpid()) + ".tsv")).string();
    std::filesystem::remove(path);
    setenv("CAJETA_XPU_REF_SURVEY", path.c_str(), 1);
    {
        Pair p(R"CJ(
    @Kernel
    public static void refSurveyRuns(KernelBuffer<int32> out) {
        out[KernelThread.globalIdX()] = 1;
    }
    @Kernel
    @Wave(width = 32)
    public static void refSurveyRefused(KernelBuffer<float32> out) {
        out[KernelThread.globalIdX()] = (float32) KernelThread.clock();
    }
)CJ", "refSurveyRuns");
        ASSERT_EQ(p.failure, "");
        std::string failure;
        for (const char* n : {"refSurveyRuns", "refSurveyRefused"}) {
            auto cpu = CpuKernel::load(findKernel(p.module, "test.M", n), failure);
            ASSERT_NE(cpu, nullptr) << failure;
        }
    }
    unsetenv("CAJETA_XPU_REF_SURVEY");
    std::ifstream in(path);
    std::stringstream all;
    all << in.rdbuf();
    std::string s = all.str();
    std::filesystem::remove(path);
    EXPECT_NE(s.find("refSurveyRuns\tRUNS\t0\n"), std::string::npos) << s;
    EXPECT_NE(s.find("refSurveyRefused\tREFUSED\t1\t"), std::string::npos) << s;
    EXPECT_NE(s.find("KernelThread.clock"), std::string::npos) << s;
}

// ---- Unit 1: the built-in families cajeta-llm's kernels use ---------------

TEST(XpuReferenceInterpreter, vectorMethodsMatchTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    public static void refVectorMethods(KernelBuffer<int32> out, KernelBuffer<float32> fout,
                                        KernelBuffer<uint8> q, KernelBuffer<int8> a,
                                        KernelBuffer<uint8> table) {
        uint32 g = KernelThread.globalIdX();
        int64 o = (int64) g * 16L;
        Vector<uint8,16> qv = q.vload<16>(o);
        Vector<int8,16> av = a.vload<16>(o);
        Vector<uint8,16> tv = table.vload<16>(0L);
        Vector<int32,4> words = qv.asWords();
        Vector<uint16,8> lo = qv.widenLo();
        Vector<uint16,8> hi = qv.widenHi();
        Vector<uint8,16> back = lo.narrow(hi);
        Vector<uint8,16> looked = qv.lut4(tv);
        Vector<int8,16> sq = qv.asSigned();
        Vector<float32,8> lf = lo.toF32();
        int32 ds = qv.dotSum(av, 7);
        Vector<int8,4> a4 = a.vload<4>(o);
        Vector<int8,4> b4 = a.vload<4>(o + 4L);
        int32 d4 = a4.dot(b4);
        uint32 b = g * 8;
        out[b] = words[1];
        out[b + 1] = (int32) hi[3] * 1000 + (int32) (back[13] & 127);
        out[b + 2] = (int32) (looked[5] & 127) + (int32) (looked[11] & 127) * 256;
        out[b + 3] = (int32) sq[2];
        out[b + 4] = ds;
        out[b + 5] = d4;
        out[b + 6] = words[3] >> 7;
        out[b + 7] = (int32) lo[7];
        fout[g] = lf[2] * 0.25f + lf[6];
    }
)CJ", "refVectorMethods");
    const uint32_t G = 16;
    std::vector<uint8_t> q(G * 16), table(16);
    std::vector<int8_t> a(G * 16);
    for (uint32_t i = 0; i < G * 16; ++i) { q[i] = (uint8_t) (i * 89 + 7); a[i] = (int8_t) (i * 41 - 70); }
    for (int i = 0; i < 16; ++i) table[i] = (uint8_t) (255 - i * 13);
    std::vector<In> ins = {buffer(std::vector<int32_t>(G * 8)), buffer(std::vector<float>(G)),
                           buffer(q), buffer(a), buffer(table)};
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(1, G)), "");
    // Bound: 0 ulp; small integers converted exactly, two roundings each side.
    EXPECT_EQ(compareOn<float>(p, ins, 1, shape(1, G)), "");
}

TEST(XpuReferenceInterpreter, mathMatchesTheCpuBackendWithinItsBound) {
    Pair p(R"CJ(
    @Kernel
    public static void refMath(KernelBuffer<float32> exact, KernelBuffer<float32> approx,
                               KernelBuffer<float32> x) {
        uint32 g = KernelThread.globalIdX();
        float32 v = x[g];
        exact[g * 4] = Math.sqrt(v * v + 1.0f);
        exact[g * 4 + 1] = Math.round(v * 3.5f) + Math.floor(v) - Math.ceil(v * 0.5f);
        exact[g * 4 + 2] = Math.fma(v, 1.5f, -2.25f);
        exact[g * 4 + 3] = Math.min(v, 0.5f) + Math.max(v, -0.5f) + Math.abs(v);
        approx[g * 2] = Math.exp(v * 0.25f);
        approx[g * 2 + 1] = Math.sin(v) + Math.cos(v * 0.5f);
    }
)CJ", "refMath");
    const uint32_t N = 64;
    std::vector<In> ins = {buffer(std::vector<float>(N * 4)), buffer(std::vector<float>(N * 2)),
                           buffer(ramp(N, 0.173f, -5.5f))};
    // Bound: 0 ulp for sqrt, round, floor, ceil, fma, min, max and abs, which
    // are exact or correctly rounded everywhere.
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(1, N)), "");
    // Bound: 4 ulp for exp and sin+cos: the reference is float64 rounded once,
    // the cpu backend's libm is within 1 ulp per call, and the sum adds one.
    EXPECT_EQ(compareOn<float>(p, ins, 1, shape(1, N), 4), "");
}

TEST(XpuReferenceInterpreter, bitCastsAndGroupPrimitivesMatchTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refGroup(KernelBuffer<int32> out, KernelBuffer<float32> fout,
                                KernelBuffer<int32> bits) {
        uint32 g = KernelThread.globalIdX();
        float32 f = Cajeta.bitsToF32(bits[g]);
        int32 back = Cajeta.f32ToBits(f * 2.0f);
        float32 s = Group.reduce(GroupOp.Add, f);
        float32 m = Group.reduce(GroupOp.Max, f);
        out[g * 3] = back;
        out[g * 3 + 1] = Group.width() * 100 + Group.laneId();
        out[g * 3 + 2] = Group.rowId();
        fout[g * 2] = s;
        fout[g * 2 + 1] = m;
    }
)CJ", "refGroup");
    const uint32_t N = 64;
    std::vector<int32_t> bits(N);
    for (uint32_t i = 0; i < N; ++i) {
        float f = (float) ((int) i - 30) * 0.5f;     // halves: every partial sum is exact
        std::memcpy(&bits[i], &f, 4);
    }
    std::vector<In> ins = {buffer(std::vector<int32_t>(N * 3)), buffer(std::vector<float>(N * 2)),
                           buffer(bits)};
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(2, 32)), "");
    // Bound: 0 ulp; halves sum exactly in any order.
    EXPECT_EQ(compareOn<float>(p, ins, 1, shape(2, 32)), "");
}

TEST(XpuReferenceInterpreter, atomicsMatchTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    public static void refAtomics(KernelBuffer<int32> hist, KernelBuffer<uint32> hi,
                                  KernelBuffer<int32> x) {
        uint32 g = KernelThread.globalIdX();
        int32 v = x[g];
        hist.atomicAdd((uint32) (v & 7), 1);
        hist.atomicMax(8, v);
        hist.atomicMin(9, v);
        hi.atomicOr(0, (uint32) 1 << (g & 31));
        hi.atomicXor(1, (uint32) v);
    }
)CJ", "refAtomics");
    const uint32_t N = 128;
    std::vector<int32_t> x(N);
    for (uint32_t i = 0; i < N; ++i) x[i] = (int32_t) (i * 2654435761u) >> 9;
    std::vector<int32_t> hist(10, 0);
    hist[9] = INT32_MAX;
    hist[8] = INT32_MIN;
    std::vector<In> ins = {buffer(hist), buffer(std::vector<uint32_t>(2)), buffer(x)};
    // Integer atomics commute, so any order gives the same answer.
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(2, 64)), "");
    EXPECT_EQ(compareOn<uint32_t>(p, ins, 1, shape(2, 64)), "");
}

// Found by the interpreter (Unit 1): a lane of an unsigned vector local,
// `(int32) v[i]`, was sign-extended on device, so a uint8 lane of 200 read
// back as -56. The host zero-extends it, and so must every backend.
TEST(XpuReferenceInterpreter, anUnsignedVectorLaneWidensWithZeroExtension) {
    Pair p(R"CJ(
    @Kernel
    public static void refLaneSign(KernelBuffer<int32> out, KernelBuffer<uint8> q,
                                   KernelBuffer<int8> s) {
        Vector<uint8,4> u = q.vload<4>(0L);
        Vector<int8,4> v = s.vload<4>(0L);
        int32 x = u[3];
        out[0] = (int32) u[1];
        out[1] = x;
        out[2] = (int32) v[1];
        out[3] = (int32) u[2] + 1;
    }
)CJ", "refLaneSign");
    std::vector<In> ins = {buffer(std::vector<int32_t>(4)),
                           buffer(std::vector<uint8_t>{1, 200, 255, 130}),
                           buffer(std::vector<int8_t>{1, -56, 3, 4})};
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(1, 1)), "");
    EXPECT_EQ(as<int32_t>(r[0].buf), (std::vector<int32_t>{200, 130, -56, 256}));
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(1, 1)), "");
}

// The two for-each forms a kernel has, `Group.stripe(n)` over a wave's lanes
// and `buf.range(n)` over the grid, and the Bits primitives.
TEST(XpuReferenceInterpreter, forEachFormsAndBitsMatchTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refStripe(KernelBuffer<float32> out, KernelBuffer<float32> x,
                                 KernelBuffer<uint32> bits, KernelBuffer<uint32> bout) {
        uint32 blk = KernelThread.globalIdX() / (uint32) Group.width();
        float32 av = 0.0f;
        for (int32 k : Group.stripe(48)) {
            float32 v = x[(int64) blk * 48L + (int64) k];
            if (v < 0.0f) { v = 0.0f - v; }
            if (v > av) { av = v; }
        }
        uint32 g = KernelThread.globalIdX();
        out[g] = Group.reduce(GroupOp.Max, av);
        uint32 w = bits[g];
        bout[g * 4] = Bits.count(w);
        bout[g * 4 + 1] = Bits.rotateLeft(w, 5);
        bout[g * 4 + 2] = Bits.reverse(w);
        bout[g * 4 + 3] = Bits.rotateRight(w, 30);
        bout[g * 4] = bout[g * 4] + Bits.rotateLeft(w, 5) % 7 + Bits.reverse(w) % 11;
    }
)CJ", "refStripe");
    const uint32_t N = 64;
    std::vector<uint32_t> bits(N);
    for (uint32_t i = 0; i < N; ++i) bits[i] = i * 2654435761u;
    std::vector<In> ins = {buffer(std::vector<float>(N)), buffer(ramp(2 * 48, -0.75f, 30.0f)),
                           buffer(bits), buffer(std::vector<uint32_t>(N * 4))};
    // Bound: 0 ulp; a max.
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(1, N)), "");
    EXPECT_EQ(compareOn<uint32_t>(p, ins, 3, shape(1, N)), "");

    Pair r(R"CJ(
    @Kernel
    public static void refRange(KernelBuffer<int32> out, KernelBuffer<int32> x, uint32 n) {
        for (uint32 i, int32 v : x.range(n)) {
            out[i] = v * 3 + (int32) i;
        }
    }
)CJ", "refRange");
    std::vector<int32_t> x(300);
    for (int i = 0; i < 300; ++i) x[i] = i * 7 - 1000;
    std::vector<In> rins = {buffer(std::vector<int32_t>(300)), buffer(x), u32(290)};
    EXPECT_EQ(compareOn<int32_t>(r, rins, 0, shape(2, 32)), "");
}

// Found by the interpreter (Unit 1): a call's result took the lowerer's
// default signedness, signed, whatever the callee declares. A uint32 from
// Bits.rotateLeft or a @Device helper with its high bit set then took a
// signed `%`, `/` or compare on device. The host uses the declared return
// type, and so must every backend.
TEST(XpuReferenceInterpreter, aCallsUnsignedResultStaysUnsigned) {
    Pair p(R"CJ(
    @Device
    public static uint32 high(uint32 v) { return v | 2147483648; }
    @Kernel
    public static void refCallSign(KernelBuffer<uint32> out, KernelBuffer<uint32> w) {
        uint32 g = KernelThread.globalIdX();
        uint32 v = w[g];
        out[g * 4] = Bits.rotateLeft(v, 5) % 7;
        out[g * 4 + 1] = high(v) / 3;
        out[g * 4 + 2] = Bits.reverse(v) >> 28;
        uint32 c = 0;
        if (high(v) > 5) { c = 1; }
        out[g * 4 + 3] = c;
    }
)CJ", "refCallSign");
    const uint32_t N = 32;
    std::vector<uint32_t> w(N);
    for (uint32_t i = 0; i < N; ++i) w[i] = i * 2654435761u;
    std::vector<In> ins = {buffer(std::vector<uint32_t>(N * 4)), buffer(w)};
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(1, N)), "");
    auto got = as<uint32_t>(r[0].buf);
    uint32_t v = w[1], rl = (v << 5) | (v >> 27);
    EXPECT_EQ(got[4], rl % 7);
    EXPECT_EQ(got[5], (v | 0x80000000u) / 3);
    EXPECT_EQ(got[7], 1u);
    EXPECT_EQ(compareOn<uint32_t>(p, ins, 0, shape(1, N)), "");
}

// Workgroups run on a pool of threads; the answer, and the error a failing
// run reports, do not depend on how many.
TEST(XpuReferenceInterpreter, parallelWorkgroupsAnswerAsOneThreadDoes) {
    Pair p(R"CJ(
    @Kernel
    public static void refGroups(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> t = shared int32[64];
        uint32 l = KernelThread.x();
        uint32 g = KernelThread.globalIdX();
        t[l] = in[g] * 7 + (int32) Workgroup.x();
        Barrier.workgroup();
        out[g] = t[63 - l] - t[(l + 5) % 64];
    }
    @Kernel
    public static void refLateFault(KernelBuffer<int32> out) {
        uint32 w = Workgroup.x();
        uint32 g = KernelThread.globalIdX();
        if (w == 5 || w == 11) { out[g + 100000] = 1; }
        out[g] = (int32) w;
    }
)CJ", "refGroups");
    ASSERT_EQ(p.failure, "");
    std::vector<int32_t> in(64 * 32);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (int32_t) (i * 2654435761u >> 11);
    std::vector<In> one = {buffer(std::vector<int32_t>(in.size())), buffer(in)}, many = one;
    setenv("CAJETA_XPU_REF_THREADS", "1", 1);
    ASSERT_EQ(runRef(p.kernel, one, shape(32, 64)), "");
    setenv("CAJETA_XPU_REF_THREADS", "8", 1);
    ASSERT_EQ(runRef(p.kernel, many, shape(32, 64)), "");
    EXPECT_EQ(as<int32_t>(one[0].buf), as<int32_t>(many[0].buf));

    auto fault = findKernel(p.module, "test.M", "refLateFault");
    for (int rep = 0; rep < 5; ++rep) {
        std::vector<In> f = {buffer(std::vector<int32_t>(16 * 64))};
        std::string why = runRef(fault, f, shape(16, 64));
        EXPECT_NE(why.find("out[100320]"), std::string::npos) << why;   // group 5, lane 0
    }
    unsetenv("CAJETA_XPU_REF_THREADS");
}

// The compiled path and the walker (CAJETA_XPU_REF_WALK=1) answer alike, bit
// for bit, over the operators, conversions and statements the compiler
// specializes.
TEST(XpuReferenceInterpreter, theCompiledPathAnswersAsTheWalkerDoes) {
    Pair p(R"CJ(
    @Device
    public static uint32 mixer(uint32 v, int32 s) { return (v >> 3) ^ (uint32) (s * 31); }
    @Kernel
    @Wave(width = 32)
    public static void refDiff(KernelBuffer<int32> out, KernelBuffer<float32> fout,
                               KernelBuffer<int32> in, KernelBuffer<float32> fin, uint32 n) {
        Shared<int32> sh = shared int32[64];
        uint32 l = KernelThread.x();
        uint32 g = KernelThread.globalIdX();
        int32 x = in[g];
        uint32 u = (uint32) x * 2654435761;
        int64 w = (int64) x * 1000003L - 7L;
        int32 acc = 0;
        for (int32 i = 0; i < 40; i++) {
            if (i == 33) { break; }
            if (i % 5 == 2) { continue; }
            acc += (x >> (i % 7)) - i * 3;
            acc ^= (int32) (u >> 11);
        }
        uint32 q = u / 13 + u % 7;
        int32 sq = x / 9 - x % 9;
        uint32 k = 0;
        while (k < 10) { k = k + 3; }
        int8 b = (int8) x;
        uint16 h = (uint16) u;
        sh[l] = acc + (int32) b + (int32) h;
        Barrier.workgroup();
        int32 nb = sh[(l + 1) % 64];
        int32 t = 0;
        for (int32 j : Group.stripe(48)) { t = t + j; }
        float32 f = fin[g];
        float32 fa = f * 1.5f - 0.25f;
        fa /= 3.0f;
        float64 d = (float64) f * 0.1 + (float64) x;
        boolean flag = (x < 0) && !(u > 4000000000);
        int32 z = 0;
        if (flag || q == 5) { z = 1; }
        out[g * 8] = acc;
        out[g * 8 + 1] = (int32) q;
        out[g * 8 + 2] = sq;
        out[g * 8 + 3] = (int32) (w >> 9);
        out[g * 8 + 4] = nb + (int32) k + z;
        out[g * 8 + 5] = t;
        out[g * 8 + 6] = (int32) mixer(u, x);
        out[g * 8 + 7] = (int32) (d * 0.25);
        fout[g] = fa + (float32) d;
    }
)CJ", "refDiff");
    ASSERT_EQ(p.failure, "");
    const uint32_t N = 128;
    std::vector<int32_t> in(N);
    std::vector<float> fin(N);
    for (uint32_t i = 0; i < N; ++i) {
        in[i] = (int32_t) (i * 2246822519u);
        fin[i] = 0.37f * (float) i - 11.0f;
    }
    std::vector<In> compiled = {buffer(std::vector<int32_t>(N * 8)), buffer(std::vector<float>(N)),
                                buffer(in), buffer(fin), u32(N)};
    std::vector<In> walked = compiled;
    ASSERT_EQ(runRef(p.kernel, compiled, shape(2, 64)), "");
    setenv("CAJETA_XPU_REF_WALK", "1", 1);
    std::string why = runRef(p.kernel, walked, shape(2, 64));
    unsetenv("CAJETA_XPU_REF_WALK");
    ASSERT_EQ(why, "");
    EXPECT_EQ(as<int32_t>(compiled[0].buf), as<int32_t>(walked[0].buf));
    EXPECT_EQ(as<uint32_t>(compiled[1].buf), as<uint32_t>(walked[1].buf));
}

// Wave.reduceSumF32 is an xor butterfly over the full width, on every
// backend (ReferenceInterpreter.md). The cpu backend summed its whole-wave
// form in lane order instead, which differs in the last bits whenever the
// order matters; these inputs make it matter.
TEST(XpuReferenceInterpreter, theFloatWaveSumIsAButterflyOnTheCpuBackend) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refWaveSumF32(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        out[g] = Wave.reduceSumF32(in[g]);
    }
)CJ", "refWaveSumF32");
    ASSERT_EQ(p.failure, "");
    const uint32_t N = 64;
    std::vector<float> in(N);
    for (uint32_t i = 0; i < N; ++i)
        in[i] = i % 4 == 0 ? 1.0e8f : i % 4 == 2 ? -1.0e8f : 1.0f + 0.25f * (float) (i % 7);

    // The instrument: on these inputs the two orders give different sums.
    float inOrder = 0.0f;
    for (uint32_t l = 0; l < 32; ++l) inOrder += in[l];
    std::vector<float> acc(in.begin(), in.begin() + 32);
    for (uint32_t d = 1; d < 32; d <<= 1) {
        std::vector<float> next(32);
        for (uint32_t l = 0; l < 32; ++l) next[l] = acc[l] + acc[l ^ d];
        acc = next;
    }
    ASSERT_NE(inOrder, acc[0]) << "the inputs do not tell the two orders apart";

    std::vector<In> ins = {buffer(std::vector<float>(N)), buffer(in)};
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(2, 32), 32), "");
    EXPECT_EQ(as<float>(r[0].buf)[0], acc[0]);
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(2, 32), 0, 32), "");
}

// Wave.reduceSumF32Segmented is the same butterfly bounded at `seg` lanes:
// each aligned span of `seg` lanes reduces on its own, and every lane of a
// span receives the span's result. The inputs make the order matter, so
// the reference has to be the butterfly and not an in-order sum.
TEST(XpuReferenceInterpreter, theSegmentedFloatWaveSumIsABoundedButterfly) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refWaveSumSeg(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        out[g] = Wave.reduceSumF32Segmented(in[g], 8);
    }
)CJ", "refWaveSumSeg");
    ASSERT_EQ(p.failure, "");
    const uint32_t N = 64;
    std::vector<float> in(N);
    // The large values differ by span (an ulp of 1e8 is 8), so spans differ.
    for (uint32_t i = 0; i < N; ++i)
        in[i] = i % 4 == 0 ? 1.0e8f + 64.0f * (float) (i / 8)
              : i % 4 == 2 ? -1.0e8f : 1.0f + 0.25f * (float) (i % 7);

    std::vector<float> acc(in.begin(), in.begin() + 32);
    for (uint32_t d = 1; d < 8; d <<= 1) {
        std::vector<float> next(32);
        for (uint32_t l = 0; l < 32; ++l) next[l] = acc[l] + acc[l ^ d];
        acc = next;
    }
    float inOrder = 0.0f;
    for (uint32_t l = 8; l < 16; ++l) inOrder += in[l];
    ASSERT_NE(inOrder, acc[8]) << "the inputs do not tell the two orders apart";

    std::vector<In> ins = {buffer(std::vector<float>(N)), buffer(in)};
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(2, 32), 32), "");
    for (uint32_t l = 0; l < 32; ++l) EXPECT_EQ(as<float>(r[0].buf)[l], acc[l]) << "lane " << l;
    EXPECT_NE(as<float>(r[0].buf)[0], as<float>(r[0].buf)[8]) << "segments were merged";
    EXPECT_EQ(compareOn<float>(p, ins, 0, shape(2, 32), 0, 32), "");
}

// BlockPadded<T, Block, Pad> is a Shared array whose element a lives at
// physical a + (a / Block) * Pad; the declared size is the physical one. The
// reference indexes logically, and the bound is on the physical slot, so a
// logical index that lands in the padding past the end is out of bounds.
TEST(XpuReferenceInterpreter, aBlockPaddedSharedArrayIndexesThroughItsPadding) {
    Pair p(R"CJ(
    @Kernel
    public static void refPadded(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        BlockPadded<int32, 8, 1> t = shared int32[72];
        uint32 l = KernelThread.x();
        t[l] = in[l] * 3;
        Barrier.workgroup();
        out[l] = t[63 - l] + t[(l + 9) % 64];
    }
    @Kernel
    public static void refPaddedOver(KernelBuffer<int32> out) {
        BlockPadded<int32, 8, 1> t = shared int32[72];
        uint32 l = KernelThread.x();
        t[l] = 1;
        if (l == 0) { t[64] = 1; }
        Barrier.workgroup();
        out[l] = t[l];
    }
)CJ", "refPadded");
    ASSERT_EQ(p.failure, "");
    std::vector<int32_t> in(64);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (int32_t) (i * 2654435761u >> 11);
    std::vector<In> ins = {buffer(std::vector<int32_t>(64)), buffer(in)};
    std::vector<In> r = ins;
    ASSERT_EQ(runRef(p.kernel, r, shape(1, 64)), "");
    for (uint32_t l = 0; l < 64; ++l)
        EXPECT_EQ(as<int32_t>(r[0].buf)[l], in[63 - l] * 3 + in[(l + 9) % 64] * 3) << l;
    EXPECT_EQ(compareOn<int32_t>(p, ins, 0, shape(1, 64), 0), "");

    auto over = findKernel(p.module, "test.M", "refPaddedOver");
    ins = {buffer(std::vector<int32_t>(64))};
    std::string why = runRef(over, ins, shape(1, 64));
    EXPECT_EQ(why.rfind("XPU-REF02", 0), 0) << why;
    EXPECT_NE(why.find("t[64]"), std::string::npos) << why;
    EXPECT_NE(why.find("out of bounds"), std::string::npos) << why;
}
