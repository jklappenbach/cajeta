// int64-scan-verbs Unit 8: `Bits` over uint64 and the two zero counts (spec 2.3, 2.4). One
// kernel takes every verb at both widths over 0, 1, the top bit alone, all ones and mixed
// words, counts a ballot and finds its lowest lane. Each lane equals a host model, every
// backend equals it, and every recorded launch replays through the reference interpreter.
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/reference/Conformance.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const uint32_t kN = 128;     // two workgroups of 64
const uint32_t kOut = 16;

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Bits;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class B {\n"
    "    static uint64[] h;\n"
    "    static int64[] hs;\n"
    "    static uint64[] got;\n"
    "    @Kernel\n"
    "    @Wave(width = 32)\n"
    "    public static void bits64(KernelBuffer<uint64> out, KernelBuffer<uint64> in,\n"
    "                              KernelBuffer<int64> sin) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        uint64 v = in[g];\n"
    "        uint32 lo = (uint32) v;\n"
    "        out[g * 16] = Bits.count(v);\n"
    "        out[g * 16 + 1] = Bits.reverse(v);\n"
    "        out[g * 16 + 2] = Bits.rotateLeft(v, 13);\n"
    "        out[g * 16 + 3] = Bits.rotateRight(v, 13);\n"
    "        out[g * 16 + 4] = Bits.trailingZeros(v);\n"
    "        out[g * 16 + 5] = Bits.leadingZeros(v);\n"
    "        out[g * 16 + 6] = Bits.count(lo);\n"
    "        out[g * 16 + 7] = Bits.trailingZeros(lo);\n"
    "        out[g * 16 + 8] = Bits.leadingZeros(lo);\n"
    "        out[g * 16 + 9] = Bits.rotateLeft(v, g % 70);\n"
    "        out[g * 16 + 10] = Bits.rotateRight(lo, g % 70);\n"
    "        uint64 b = Wave.ballotSync(lo % 3 == 0);\n"
    "        out[g * 16 + 11] = Bits.count(b);\n"
    "        out[g * 16 + 12] = Bits.trailingZeros(b);\n"
    "        out[g * 16 + 13] = Bits.trailingZeros(Wave.ballotSync(g > 100000));\n"
    "        out[g * 16 + 14] = Bits.count(sin[g]);\n"
    "        out[g * 16 + 15] = Bits.reverse(lo);\n"
    "    }\n"
    "    public static void init(uint32 n) {\n"
    "        h = heap uint64[n];\n"
    "        hs = heap int64[n];\n"
    "    }\n"
    "    public static void put(uint32 i, uint64 v, int64 s) { h[i] = v; hs[i] = s; }\n"
    "    public static uint32 hostCountI64(int64 s) { return Bits.count(s); }\n"
    "    public static uint64 hostVerb(uint32 k, uint64 v, uint32 amt) {\n"
    "        uint32 lo = (uint32) v;\n"
    "        if (k == 0) { return Bits.count(v); }\n"
    "        if (k == 1) { return Bits.reverse(v); }\n"
    "        if (k == 2) { return Bits.rotateLeft(v, amt); }\n"
    "        if (k == 3) { return Bits.rotateRight(v, amt); }\n"
    "        if (k == 4) { return Bits.trailingZeros(v); }\n"
    "        if (k == 5) { return Bits.leadingZeros(v); }\n"
    "        if (k == 6) { return Bits.count(lo); }\n"
    "        if (k == 7) { return Bits.trailingZeros(lo); }\n"
    "        if (k == 8) { return Bits.leadingZeros(lo); }\n"
    "        if (k == 9) { return Bits.rotateRight(lo, amt); }\n"
    "        return Bits.reverse(lo);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 128;\n"
    "        KernelBuffer<uint64> in = heap KernelBuffer<uint64>(n);\n"
    "        in.upload(h);\n"
    "        KernelBuffer<int64> sin = heap KernelBuffer<int64>(n);\n"
    "        sin.upload(hs);\n"
    "        KernelBuffer<uint64> o = heap KernelBuffer<uint64>(n * 16);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        try {\n"
    "            bits64.launch(s, grid: [2], block: [64])(o, in, sin);\n"
    "            s.sync();\n"
    "        } catch (Exception e) {\n"
    "            System.stderr.println(\"launch threw: \" + e.getMessage());\n"
    "            return 2;\n"
    "        }\n"
    "        got = heap uint64[n * 16];\n"
    "        o.download(got);\n"
    "        return 1;\n"
    "    }\n"
    "    public static uint64 at(uint32 i) { return got[i]; }\n"
    "}\n";

uint64_t input(uint32_t g) {
    uint64_t mix = (uint64_t) (g + 1) * UINT64_C(0x9E3779B97F4A7C15);
    switch (g % 8) {
        case 0: return 0;
        case 1: return 1;
        case 2: return UINT64_C(1) << 63;
        case 3: return ~UINT64_C(0);
        case 4: return mix;
        case 5: return UINT64_C(1) << 32;
        case 6: return UINT64_C(0xFFFFFFFF);
        default: return mix << 32;
    }
}

int64_t signedInput(uint32_t g) { return (int64_t) input(g) - (int64_t) g * 977; }

uint64_t rev64(uint64_t v) {
    uint64_t r = 0;
    for (int b = 0; b < 64; ++b) if (v >> b & 1) r |= UINT64_C(1) << (63 - b);
    return r;
}
uint32_t rev32(uint32_t v) { return (uint32_t) (rev64(v) >> 32); }
uint64_t rotl64(uint64_t v, uint32_t s) { s &= 63; return s ? (v << s) | (v >> (64 - s)) : v; }
uint64_t rotr64(uint64_t v, uint32_t s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
uint32_t rotr32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
uint32_t tz64(uint64_t v) { return v ? (uint32_t) __builtin_ctzll(v) : 64; }
uint32_t lz64(uint64_t v) { return v ? (uint32_t) __builtin_clzll(v) : 64; }
uint32_t tz32(uint32_t v) { return v ? (uint32_t) __builtin_ctz(v) : 32; }
uint32_t lz32(uint32_t v) { return v ? (uint32_t) __builtin_clz(v) : 32; }

// The host model of each output at a 32-lane wave. Output 14, the int64 argument, is held
// to the host's own call instead, so the device agrees with whatever overload the host picks.
std::vector<uint64_t> model() {
    std::vector<uint64_t> out(kN * kOut);
    for (uint32_t g = 0; g < kN; ++g) {
        uint64_t v = input(g);
        uint32_t lo = (uint32_t) v;
        uint32_t base = g - g % 32;
        uint64_t ballot = 0;
        for (uint32_t j = 0; j < 32; ++j)
            if ((uint32_t) input(base + j) % 3 == 0) ballot |= UINT64_C(1) << j;
        uint64_t* o = &out[g * kOut];
        o[0] = (uint64_t) __builtin_popcountll(v);
        o[1] = rev64(v);
        o[2] = rotl64(v, 13);
        o[3] = rotr64(v, 13);
        o[4] = tz64(v);
        o[5] = lz64(v);
        o[6] = (uint64_t) __builtin_popcount(lo);
        o[7] = tz32(lo);
        o[8] = lz32(lo);
        o[9] = rotl64(v, g % 70);
        o[10] = rotr32(lo, g % 70);
        o[11] = (uint64_t) __builtin_popcountll(ballot);
        o[12] = tz64(ballot);
        o[13] = 64;
        o[14] = 0;
        o[15] = rev32(lo);
    }
    return out;
}

struct Outputs {
    std::vector<uint64_t> got;
    std::vector<uint64_t> hostI64;
    std::string stderrText;
    bool ran = false;
};

Outputs runOn(cajeta::xpu::Backend be, const fs::path& dir) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    opts.xpuKernelGateErrors = true;
    auto jit = CajetaJit::compile(kSource, "test.B", opts);
    EXPECT_NE(jit, nullptr) << "the uint64 Bits forms do not compile";
    if (!jit) return o;
    auto init = jit->lookup<void (*)(uint32_t)>("init");
    auto put = jit->lookup<void (*)(uint32_t, uint64_t, int64_t)>("put");
    auto host = jit->lookup<uint32_t (*)(int64_t)>("hostCountI64");
    auto run = jit->lookup<int32_t (*)()>("run");
    auto at = jit->lookup<uint64_t (*)(uint32_t)>("at");
    EXPECT_TRUE(init && put && host && run && at);
    if (!(init && put && host && run && at)) return o;
    init(kN);
    for (uint32_t g = 0; g < kN; ++g) {
        put(g, input(g), signedInput(g));
        o.hostI64.push_back(host(signedInput(g)));
    }
    if (!dir.empty()) setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    testing::internal::CaptureStderr();
    int32_t r = run();
    o.stderrText = testing::internal::GetCapturedStderr();
    if (!dir.empty()) unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1) << o.stderrText;
    if (r != 1) return o;
    for (uint32_t k = 0; k < kN * kOut; ++k) o.got.push_back(at(k));
    o.ran = true;
    return o;
}

const char* kOutName[] = {"count u64", "reverse u64", "rotateLeft u64", "rotateRight u64",
                          "trailingZeros u64", "leadingZeros u64", "count u32",
                          "trailingZeros u32", "leadingZeros u32", "rotateLeft u64 by g",
                          "rotateRight u32 by g", "count of a ballot",
                          "trailingZeros of a ballot", "trailingZeros of an empty ballot",
                          "count of an int64", "reverse u32"};

// 2.4.1 to 2.4.3: each lane equals the model, and an int64 argument equals the host's call.
void matchesTheModel(const Outputs& o, const char* backend) {
    ASSERT_TRUE(o.ran);
    EXPECT_EQ(o.stderrText.find("[xpu-kernel-skipped]"), std::string::npos)
        << backend << " did not run the kernel:\n" << o.stderrText;
    std::vector<uint64_t> want = model();
    ASSERT_EQ(o.got.size(), want.size());
    uint32_t bad = 0;
    for (uint32_t k = 0; k < want.size(); ++k) {
        uint32_t g = k / kOut;
        uint64_t w = k % kOut == 14 ? o.hostI64[g] : want[k];
        if (o.got[k] == w) continue;
        if (++bad <= 12)
            ADD_FAILURE() << kOutName[k % kOut] << " at g " << g << " (input 0x" << std::hex
                << input(g) << "): got 0x" << o.got[k] << ", want 0x" << w << std::dec
                << " on " << backend;
    }
    EXPECT_EQ(bad, 0u) << "outputs differ from the model on " << backend << "\n" << o.stderrText;
}

void everyLaunchReplays(const fs::path& dir, const char* backend) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource, "test.B");
    auto k = findKernel(module, "test.B", "bits64");
    ASSERT_NE(k, nullptr);
    ref::CorpusRun run = ref::runCorpus(std::vector<cajeta::MethodPtr>{k}, dir.string(), "");
    bool seen = false;
    for (auto& r : run.results) {
        if (r.kernel.find("bits64") != std::string::npos) seen = true;
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    }
    EXPECT_TRUE(seen) << "bits64 has no recording on " << backend;
}

void deviceMatches(cajeta::xpu::Backend be, const char* backend, bool record) {
    fs::path dir;
    if (record) {
        dir = fs::temp_directory_path() / ("cajeta-bits-u64-" + std::string(backend));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    matchesTheModel(runOn(be, dir), backend);
    if (record) {
        everyLaunchReplays(dir, backend);
        fs::remove_all(dir);
    }
}

std::string lowerOnCpu(const std::string& source, const std::string& kernel) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, source, "test.N");
    auto k = findKernel(module, "test.N", kernel);
    EXPECT_NE(k, nullptr);
    if (!k) return "";
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_bits_width", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    EXPECT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);
    std::string s;
    llvm::raw_string_ostream os(s);
    host.print(os, nullptr);
    return os.str();
}

}  // namespace

TEST(XpuBitsU64, everyFormMatchesTheHostModelOnCpu) {
    matchesTheModel(runOn(cajeta::xpu::Backend::Cpu, fs::path()), "cpu");
}

TEST(XpuBitsU64, everyLaunchReplaysThroughTheReferenceOnCpu) {
    fs::path dir = fs::temp_directory_path() / "cajeta-bits-u64-cpu";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Outputs o = runOn(cajeta::xpu::Backend::Cpu, dir);
    ASSERT_TRUE(o.ran);
    everyLaunchReplays(dir, "cpu");
    fs::remove_all(dir);
}

// The host forms, called outside a kernel, equal the same model.
TEST(XpuBitsU64, theHostFormsMatchTheModel) {
    auto jit = CajetaJit::compile(kSource, "test.B", CajetaJit::Options{});
    ASSERT_NE(jit, nullptr);
    auto verb = jit->lookup<uint64_t (*)(uint32_t, uint64_t, uint32_t)>("hostVerb");
    ASSERT_NE(verb, nullptr);
    for (uint32_t g = 0; g < kN; ++g) {
        uint64_t v = input(g);
        uint32_t lo = (uint32_t) v;
        uint32_t amt = g % 70;
        uint64_t want[] = {(uint64_t) __builtin_popcountll(v), rev64(v), rotl64(v, amt),
                           rotr64(v, amt), tz64(v), lz64(v), (uint64_t) __builtin_popcount(lo),
                           tz32(lo), lz32(lo), rotr32(lo, amt), rev32(lo)};
        for (uint32_t k = 0; k < 11; ++k)
            EXPECT_EQ(verb(k, v, amt), want[k]) << "verb " << k << " at input 0x" << std::hex << v;
    }
}

// 2.4.4: a uint32, an int32 and a literal still call the 32-bit form, and only a uint64
// reaches the 64-bit one.
TEST(XpuBitsU64, narrowArgumentsKeepTheThirtyTwoBitForm) {
    const std::string narrow =
        "package test;\n"
        "import cajeta.xpu.Bits;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class N {\n"
        "    @Kernel\n"
        "    public static void narrow(KernelBuffer<uint32> out, KernelBuffer<int32> in) {\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        int32 s = in[g];\n"
        "        uint32 u = (uint32) s;\n"
        "        out[g * 3] = Bits.count(u);\n"
        "        out[g * 3 + 1] = Bits.count(s);\n"
        "        out[g * 3 + 2] = Bits.count(7);\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void wide(KernelBuffer<uint32> out, KernelBuffer<uint64> in) {\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        out[g] = Bits.count(in[g]);\n"
        "    }\n"
        "}\n";
    std::string ir = lowerOnCpu(narrow, "narrow");
    EXPECT_EQ(ir.find("llvm.ctpop.i64"), std::string::npos) << ir;
    std::string wide = lowerOnCpu(narrow, "wide");
    EXPECT_NE(wide.find("llvm.ctpop.i64"), std::string::npos) << wide;
}

TEST(XpuBitsU64, everyFormMatchesTheModelAndReplaysOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    deviceMatches(cajeta::xpu::Backend::Nvptx, "nvptx", true);
}

TEST(XpuBitsU64, everyFormMatchesTheModelAndReplaysOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    deviceMatches(cajeta::xpu::Backend::Amdgpu, "amdgpu", true);
}

// Vulkan is not recorded (its handles are not addresses), so it is held to the model only.
TEST(XpuBitsU64, everyFormMatchesTheModelOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    deviceMatches(cajeta::xpu::Backend::Spirv, "vulkan", false);
}
