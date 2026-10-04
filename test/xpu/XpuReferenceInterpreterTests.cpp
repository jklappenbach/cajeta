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
import cajeta.xpu.CooperativeMatrix;
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

TEST(XpuReferenceInterpreter, anOperationWithNoReferenceSemanticsIsRefusedByName) {
    Pair p(R"CJ(
    @Kernel
    @Wave(width = 32)
    public static void refRefused(KernelBuffer<float32> out) {
        uint32 g = KernelThread.globalIdX();
        out[g] = 7.0f;
        out[g] = Wave.reduceSumF32Segmented(out[g], 8);
    }
)CJ", "refRefused");
    ASSERT_EQ(p.failure, "");
    auto why = ref::refusals(p.kernel);
    ASSERT_EQ(why.size(), 1);
    EXPECT_NE(why[0].find("Wave.reduceSumF32Segmented"), std::string::npos) << why[0];
    EXPECT_NE(why[0].find("line"), std::string::npos) << why[0];

    std::vector<In> ins = {buffer(std::vector<float>(32, -1.0f))};
    std::string run = runRef(p.kernel, ins, shape(1, 32));
    EXPECT_EQ(run.rfind("XPU-REF01", 0), 0) << run;
    EXPECT_NE(run.find("Wave.reduceSumF32Segmented"), std::string::npos) << run;
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
