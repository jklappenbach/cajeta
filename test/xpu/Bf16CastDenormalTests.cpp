// `(bfloat16) x` keeps subnormals on every x86 CPU (specs/bf16-cast-denormals-spec.md).
//
// AVX512-BF16's vcvtneps2bf16 and AVX-NE-CONVERT's form treat a float32 denormal
// input as zero, and LLVM chose them for every float32-to-bf16 narrowing on a CPU
// that has them (proton's Zen 5: 1.0e-40 gave 0x0000, not 0x0001). The cajeta-llvm
// fork's x86 lowering now uses them only where the function lets float32 denormals
// flush, which a @FastMath cpu kernel does (r13).
//
// Two kinds of test. The assembly test builds an executable for -mcpu=znver5 and
// reads which functions hold the instruction, so it runs on any x86 box. The value
// tests run at the host's own CPU: on proton they are the red tests, on a CPU
// without the instructions (Phoenix) a guard.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>

using cajeta_test::CajetaJit;
namespace fs = std::filesystem;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Vector;
public class B {
    // One narrowing per work-item, and eight at a time through Vector.toBF16.
    @Kernel @Occupancy(maxThreads = 32)
    public static void narrow(KernelBuffer<bfloat16> y, KernelBuffer<float32> x) {
        uint32 i = KernelThread.globalIdX();
        y[i] = (bfloat16) x[i];
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void narrowVec(KernelBuffer<bfloat16> y, KernelBuffer<float32> x) {
        uint32 g = KernelThread.globalIdX();
        Vector<float32,8> v = x.vload<8>((int64) g * 8L);
        y.vstore((int64) g * 8L, v.toBF16());
    }
    // bf16 arithmetic rounds back to bf16 inside LLVM's instruction selection.
    @Kernel @Occupancy(maxThreads = 32)
    public static void mul(KernelBuffer<bfloat16> y, KernelBuffer<bfloat16> a, KernelBuffer<bfloat16> b) {
        uint32 i = KernelThread.globalIdX();
        y[i] = a[i] * b[i];
    }
    // @FastMath may flush, so it may keep the one-instruction convert.
    @Kernel @FastMath @Occupancy(maxThreads = 32)
    public static void fastNarrow(KernelBuffer<bfloat16> y, KernelBuffer<float32> x) {
        uint32 i = KernelThread.globalIdX();
        y[i] = (bfloat16) x[i];
    }

    public static float32 hostCast(float32 x) { return (float32) ((bfloat16) x); }
    public static float32 hostCastF64(float64 x) { return (float32) ((bfloat16) x); }
    public static float32 hostMul(float32 a, float32 b) {
        bfloat16 p = (bfloat16) a * (bfloat16) b;
        return (float32) p;
    }

    // which 0: narrow, 1: narrowVec. The 32 inputs come from the host; the
    // widened results go back for the bits.
    public static float32 kernelCast(int32 which, int32 at, float32 v0, float32 v1, float32 v2, float32 v3) {
        float32[] h = heap float32[32];
        for (int32 i = 0; i < 32; i = i + 1) { h[i] = 0.0f; }
        h[0] = v0; h[1] = v1; h[2] = v2; h[3] = v3;
        h[8] = v0; h[17] = v1; h[26] = v2; h[31] = v3;
        bfloat16[] o = heap bfloat16[32];
        KernelBuffer<float32> x = heap KernelBuffer<float32>(32);
        KernelBuffer<bfloat16> y = heap KernelBuffer<bfloat16>(32);
        x.upload(h);
        KernelStream s #= KernelStream.current();
        if (which == 0) { narrow.launch(s, grid: [1], block: [32])(y, x); }
        if (which == 1) { narrowVec.launch(s, grid: [1], block: [4])(y, x); }
        s.sync();
        y.download(o);
        return (float32) o[at];
    }
    public static float32 kernelMul(float32 a0, float32 b0) {
        bfloat16[] ha = heap bfloat16[32];
        bfloat16[] hb = heap bfloat16[32];
        for (int32 i = 0; i < 32; i = i + 1) { ha[i] = (bfloat16) a0; hb[i] = (bfloat16) b0; }
        bfloat16[] o = heap bfloat16[32];
        KernelBuffer<bfloat16> a = heap KernelBuffer<bfloat16>(32);
        KernelBuffer<bfloat16> b = heap KernelBuffer<bfloat16>(32);
        KernelBuffer<bfloat16> y = heap KernelBuffer<bfloat16>(32);
        a.upload(ha);
        b.upload(hb);
        KernelStream s #= KernelStream.current();
        mul.launch(s, grid: [1], block: [32])(y, a, b);
        s.sync();
        y.download(o);
        return (float32) o[5];
    }
    public static int32 main() { return 0; }
}
)CJ";

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float fromBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// float32 input bits -> the bf16 that IEEE round-to-nearest-even gives.
struct Case { uint32_t in; uint16_t want; const char* what; };
const Case kCases[] = {
    {0x000116C2u, 0x0001, "1.0e-40, a float32 denormal: the bf16 subnormal 0x0001"},
    {0x800116C2u, 0x8001, "-1.0e-40"},
    {0x000AE398u, 0x000B, "1.0e-39, rounding up into 0x000B"},
    {0x00000001u, 0x0000, "the smallest denormal rounds to zero"},
    {0x3F808000u, 0x3F80, "1 + 2^-8, a tie, to even"},
    {0x3F818000u, 0x3F82, "1 + 3 * 2^-8, a tie, to even"},
    {0x7F7FFFFFu, 0x7F80, "the largest float32 rounds to infinity"},
    {0x7FC00000u, 0x7FC0, "a quiet NaN"},
    {0x80000000u, 0x8000, "negative zero"},
};

std::string compilerPath() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    return std::string(root && *root ? root : ".") + "/build/src/cajeta";
}

std::string llvmBin() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    return std::string(root && *root ? root : ".") + "/../cajeta-llvm/llvm-install/bin";
}

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cajeta_shell(cmd + " 2>&1").c_str(), "r");
    if (!p) return out;
    std::array<char, 4096> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
    pclose(p);
    return out;
}

} // namespace

// Built for Zen 5, the only functions that convert with vcvtneps2bf16 are @FastMath
// ones; the host casts, Vector.toBF16, the cpu kernels and bf16 arithmetic do not.
TEST(Bf16CastDenormal, onlyFastMathCodeUsesTheFlushingConvertAtZnver5) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path base = fs::temp_directory_path() / ("cajeta_bf16dn_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "test");
    fs::create_directories(base / "out");
    std::ofstream(base / "src" / "test" / "B.cajeta") << kSource;
    const std::string exe = (base / "out" / "b").string();
    std::string built = run("CAJETA_XPU_CPU_MCPU=znver5 \"" + compilerPath() +
                            "\" --emit=exe --cpu=znver5 --xpu-backend=cpu -o \"" + exe +
                            "\" test.B.main \"" + (base / "src").string() + "\" \"" +
                            (base / "out").string() + "\"");
    ASSERT_TRUE(fs::exists(exe)) << built;
    std::string dis = run("\"" + llvmBin() + "/llvm-objdump\" -d --no-show-raw-insn \"" + exe + "\"");
    std::error_code ec;
    fs::remove_all(base, ec);
    ASSERT_FALSE(dis.empty());
    std::set<std::string> users;
    std::string fn;
    std::istringstream in(dis);
    for (std::string line; std::getline(in, line);) {
        auto lt = line.find(" <");
        if (lt != std::string::npos && line.size() > 2 && line.substr(line.size() - 2) == ">:")
            fn = line.substr(lt + 2, line.size() - lt - 4);
        else if (line.find("vcvtneps2bf16") != std::string::npos)
            users.insert(fn);
    }
    bool fast = false;
    for (const std::string& u : users) {
        if (u.find("fastNarrow") != std::string::npos) { fast = true; continue; }
        ADD_FAILURE() << u << " converts with vcvtneps2bf16, which flushes denormals";
    }
    EXPECT_TRUE(fast) << "the @FastMath kernel should keep the one-instruction convert";
}

// The values at the host's own CPU: the cast, the float64 cast, the cpu kernels and
// bf16 arithmetic all round as IEEE does.
TEST(Bf16CastDenormal, everyNarrowingKeepsSubnormalsOnThisCpu) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.B", o);
    ASSERT_NE(jit, nullptr);
    auto cast = jit->lookup<float (*)(float)>("hostCast");
    auto kern = jit->lookup<float (*)(int32_t, int32_t, float, float, float, float)>("kernelCast");
    ASSERT_TRUE(cast && kern);
    for (const Case& c : kCases) {
        const uint32_t want = (uint32_t) c.want << 16;
        EXPECT_EQ(want, bitsOf(cast(fromBits(c.in)))) << "host cast: " << c.what;
    }
    // Four inputs a launch, at cells 0-3 (and again at 8, 17, 26, 31 for the
    // vector kernel's other work-items).
    for (size_t g = 0; g < sizeof(kCases) / sizeof(kCases[0]); g += 4) {
        float v[4];
        uint32_t want[4];
        for (int k = 0; k < 4; ++k) {
            const Case& c = kCases[(g + k) % (sizeof(kCases) / sizeof(kCases[0]))];
            v[k] = fromBits(c.in);
            want[k] = (uint32_t) c.want << 16;
        }
        for (int which = 0; which < 2; ++which)
            for (int k = 0; k < 4; ++k) {
                const int at[2][4] = {{0, 1, 2, 3}, {8, 17, 26, 31}};
                EXPECT_EQ(want[k], bitsOf(kern(which, at[which & 1][k], v[0], v[1], v[2], v[3])))
                    << (which ? "Vector.toBF16" : "the kernel cast") << ", input "
                    << std::hex << bitsOf(v[k]);
                EXPECT_EQ(want[k], bitsOf(kern(which, at[0][k], v[0], v[1], v[2], v[3])))
                    << (which ? "Vector.toBF16" : "the kernel cast") << " at cell " << k;
            }
    }
}

// bf16 arithmetic whose product is a bf16 subnormal: 2^-63 * 2^-63 = 2^-126 is the
// smallest normal; 2^-64 * 2^-64 = 2^-128 is the subnormal 0x0020.
TEST(Bf16CastDenormal, bf16ArithmeticKeepsASubnormalProduct) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.B", o);
    ASSERT_NE(jit, nullptr);
    auto host = jit->lookup<float (*)(float, float)>("hostMul");
    auto kern = jit->lookup<float (*)(float, float)>("kernelMul");
    ASSERT_TRUE(host && kern);
    const float a = std::ldexp(1.0f, -64);
    EXPECT_EQ(0x00200000u, bitsOf(host(a, a))) << "host bf16 multiply";
    EXPECT_EQ(0x00200000u, bitsOf(kern(a, a))) << "cpu kernel bf16 multiply";
}

// float64 to bf16 rounds once (spec decision 4): 1 + 2^-8 + 2^-40 is just above the
// tie between 1 and 1 + 2^-7, so it rounds up, where narrowing through float32
// first would land on the tie and round to even, down to 1.
TEST(Bf16CastDenormal, float64NarrowsWithOneRounding) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.B", o);
    ASSERT_NE(jit, nullptr);
    auto cast = jit->lookup<float (*)(double)>("hostCastF64");
    ASSERT_TRUE(cast);
    const double d = 1.0 + std::ldexp(1.0, -8) + std::ldexp(1.0, -40);
    EXPECT_EQ(0x3F810000u, bitsOf(cast(d))) << "1 + 2^-8 + 2^-40 rounds up to 1 + 2^-7";
}
