// cpu-isa-variants Unit 2: `--cpu=portable` emits every cpu kernel once per ISA
// level and the host module at the x86-64-v3 baseline; the runtime resolves each
// kernel to the highest level the host supports (XpuCpuLevelTests holds the
// resolver itself). Two instruments: the IR of an in-process compile, for the
// symbols and the per-function target attributes; and an AOT probe run under
// CAJETA_XPU_CPU_LEVEL, for the level each copy reports and the bits it computes.
#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../PortableEnv.h"

using cajeta::Compiler;
using cajeta::EmitMode;
using cajeta::XpuBackend;

namespace {
namespace fs = std::filesystem;

// A kernel with a 32-pair integer dot on masked nibbles (the shape the quantized
// kernels take), so each level's copy differs in what dotAccum lowers to.
const char* kProbe = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
import cajeta.xpu.Vector;
public class P {
    @Kernel
    public static void dot(KernelBuffer<int32> out, KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) {
            int64 o = (int64) g * 32L;
            Vector<uint8,32> wv = w.vload<32>(o).asUnsigned() & 15;
            Vector<int8,32> xv = x.vload<32>(o);
            Vector<int32,8> z = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);
            Vector<int32,8> acc = wv.dotAccum(xv, z);
            out[g] = acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
        }
    }
    public static void run() {
        uint32 n = 2048;
        int8[] hw #= heap int8[n * 32];
        int8[] hx #= heap int8[n * 32];
        int64 i = 0;
        while (i < (int64) (n * 32)) {
            hw[i] = (int8) ((i * 7L + 3L) % 256L - 128L);
            hx[i] = (int8) ((i * 13L + 5L) % 256L - 128L);
            i = i + 1;
        }
        KernelBuffer<int8> w = heap KernelBuffer<int8>(n * 32);
        KernelBuffer<int8> x = heap KernelBuffer<int8>(n * 32);
        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);
        w.upload(hw);
        x.upload(hx);
        KernelStream s #= KernelStream.current();
        dot.launch(s, grid: [n / 64], block: [64])(out, w, x, n);
        s.sync();
        int32[] ho #= heap int32[n];
        out.download(ho);
        int64 sum = 0;
        uint32 k = 0;
        while (k < n) { sum = sum * 31L + (int64) ho[k]; sum = sum % 1000000007L; k = k + 1; }
        System.stdout.println("checksum " + sum);
        System.stdout.println("level " + Device.cpuLevelName(Device.cpuLevel()));
        System.stdout.println("kernel " + Device.cpuLevelName(Device.cpuKernelLevel("dot")));
        System.stdout.println("wave " + Device.waveSize());
        return;
    }
}
)CJ";

const char* kSaxpy =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "}\n";

std::string compilerPath() {
    std::string r;
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

std::string capture(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cajeta_shell(cmd + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str(), "r");
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
    pclose(p);
    return out;
}

fs::path freshDir(const char* tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / (std::string("cajeta_portable_") + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Builds the probe once with `--cpu=portable`; returns the exe path ("" on failure).
std::string buildProbe(const fs::path& base, std::string* log) {
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "P.cajeta") << kProbe;
    const std::string exe = (base / "out").string();
    const std::string cmd = "\"" + compilerPath() + "\" --cpu=portable --release --emit=exe --xpu-backend=cpu"
        + " -o \"" + exe + "\" probe.P.run \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    *log = capture(cmd + " 2>&1");
    return fs::exists(cajeta_exe_path(exe)) ? cajeta_exe_path(exe).string() : std::string();
}

std::string field(const std::string& out, const std::string& label) {
    const size_t p = out.find(label + " ");
    if (p == std::string::npos) return "";
    const size_t e = out.find('\n', p);
    return out.substr(p + label.size() + 1, e == std::string::npos ? std::string::npos : e - p - label.size() - 1);
}

// `tcpu`, when given, receives the host module's effective target cpu: the codegen
// default for every function without its own `target-cpu`.
std::string irOf(const std::string& cpu, const std::vector<std::string>& variants, const char* tag,
                 std::string* tcpu = nullptr) {
    auto base = freshDir(tag);
    auto src = base / "src";
    fs::create_directories(src / "test");
    fs::create_directories(base / "build");
    std::ofstream(src / "test" / "M.cajeta") << kSaxpy;
    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Cpu);
    compiler.setCpu(cpu);
    if (!variants.empty()) compiler.setCpuVariants(variants);
    compiler.compile("test.M.saxpy", src.string(), (base / "build").string());
    if (tcpu && compiler.getTargetMachine()) *tcpu = compiler.getTargetMachine()->getTargetCPU().str();
    std::vector<fs::path> hits;
    for (auto& e : fs::recursive_directory_iterator(base / "build"))
        if (e.is_regular_file() && e.path().extension() == ".ll") hits.push_back(e.path());
    std::sort(hits.begin(), hits.end());
    std::string ir;
    for (auto& p : hits) {
        if (p.filename().string().find("__stdlib__") != std::string::npos) continue;
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        ir = ss.str();
        break;
    }
    fs::remove_all(base);
    return ir;
}

// The text of the function `name` in `ir`, from its `define` to its closing brace.
std::string functionText(const std::string& ir, const std::string& name) {
    const size_t d = ir.find("define");
    size_t p = d;
    while (p != std::string::npos) {
        size_t eol = ir.find('\n', p);
        std::string head = ir.substr(p, eol - p);
        if (head.find("@" + name + "(") != std::string::npos || head.find("@\"" + name + "\"(") != std::string::npos) {
            size_t end = ir.find("\n}", p);
            return ir.substr(p, end == std::string::npos ? std::string::npos : end + 2 - p);
        }
        p = ir.find("\ndefine", eol);
        if (p != std::string::npos) ++p;
    }
    return "";
}

int count(const std::string& s, const std::string& needle) {
    int n = 0;
    for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + needle.size())) ++n;
    return n;
}
}  // namespace

// 2.1.1 (IR half): four copies of the kernel, each registered at its level, each
// carrying its level's target; the host module at the x86-64-v3 baseline.
TEST(XpuCpuPortable, portableEmitsEveryLevelAndABaselineHost) {
    std::string tcpu;
    const std::string ir = irOf("portable", {}, "ir4", &tcpu);
    ASSERT_FALSE(ir.empty()) << "no .ll written";
    // The host module compiles at the baseline: its functions carry no target-cpu of
    // their own (that is stamped only for ThinLTO), so codegen takes the machine's.
    EXPECT_EQ(tcpu, "x86-64-v3");
    for (const char* lvl : {"v3", "v3-vnni", "v4", "v4-amx"}) {
        EXPECT_NE(ir.find(std::string("__cajeta_xpu_cpu_launch.test.M.saxpy.") + lvl), std::string::npos) << lvl;
    }
    EXPECT_EQ(count(ir, "call void @__cajeta_xpu_register_cpu_kernel_at("), 4) << "one registration a level";
    EXPECT_EQ(count(ir, "call void @__cajeta_xpu_register_cpu_kernel("), 0) << "no host-level copy in a portable build";
    const std::string v3 = functionText(ir, "__cajeta_xpu_cpu_block.test.M.saxpy.v3");
    const std::string v4 = functionText(ir, "__cajeta_xpu_cpu_block.test.M.saxpy.v4");
    const std::string vnni = functionText(ir, "__cajeta_xpu_cpu_block.test.M.saxpy.v3-vnni");
    const std::string amx = functionText(ir, "__cajeta_xpu_cpu_block.test.M.saxpy.v4-amx");
    ASSERT_FALSE(v3.empty());
    ASSERT_FALSE(v4.empty());
    // Attributes sit in an #N group; find the group each function names and read it.
    auto attrsOf = [&](const std::string& fn) {
        const size_t h = fn.find('{');
        const std::string head = fn.substr(0, h);
        const size_t a = head.rfind('#');
        if (a == std::string::npos) return std::string();
        const std::string id = head.substr(a, head.find_first_of(" \n", a) - a);
        const size_t g = ir.find("attributes " + id + " = ");
        if (g == std::string::npos) return std::string();
        return ir.substr(g, ir.find('\n', g) - g);
    };
    EXPECT_NE(attrsOf(v3).find("\"target-cpu\"=\"x86-64-v3\""), std::string::npos) << attrsOf(v3);
    EXPECT_EQ(attrsOf(v3).find("avx512"), std::string::npos) << attrsOf(v3);
    EXPECT_NE(attrsOf(vnni).find("+avxvnni"), std::string::npos) << attrsOf(vnni);
    EXPECT_NE(attrsOf(v4).find("\"target-cpu\"=\"x86-64-v4\""), std::string::npos) << attrsOf(v4);
    EXPECT_NE(attrsOf(v4).find("+avx512vnni"), std::string::npos) << attrsOf(v4);
    EXPECT_NE(attrsOf(amx).find("+amx-int8"), std::string::npos) << attrsOf(amx);
    // The host module's own functions carry no level: the ctor has none of the v4 features.
    const std::string host = functionText(ir, "__cajeta_xpu_cpu_reg_ctor.test.M.saxpy.v3");
    ASSERT_FALSE(host.empty());
    EXPECT_EQ(attrsOf(host).find("avx512"), std::string::npos) << attrsOf(host);
}

// 2.1.2: `native` is one copy under today's symbols; `--cpu-variants` picks a subset.
TEST(XpuCpuPortable, nativeKeepsOneCopyAndVariantsPickASubset) {
    std::string tcpu;
    const std::string nat = irOf("native", {}, "irnat", &tcpu);
    ASSERT_FALSE(nat.empty());
    EXPECT_NE(tcpu, "x86-64-v3") << "native keeps the host's cpu";
    EXPECT_FALSE(tcpu.empty());
    EXPECT_EQ(count(nat, "call void @__cajeta_xpu_register_cpu_kernel("), 1);
    EXPECT_EQ(count(nat, "call void @__cajeta_xpu_register_cpu_kernel_at("), 0);
    EXPECT_NE(nat.find("__cajeta_xpu_cpu_launch.test.M.saxpy("), std::string::npos);
    EXPECT_EQ(nat.find("saxpy.v3"), std::string::npos);
    const std::string two = irOf("portable", {"v3", "v4"}, "ir2");
    ASSERT_FALSE(two.empty());
    EXPECT_EQ(count(two, "call void @__cajeta_xpu_register_cpu_kernel_at("), 2);
    EXPECT_NE(two.find("saxpy.v3"), std::string::npos);
    EXPECT_NE(two.find("saxpy.v4"), std::string::npos);
    EXPECT_EQ(two.find("saxpy.v3-vnni"), std::string::npos);
    EXPECT_EQ(two.find("saxpy.v4-amx"), std::string::npos);
}

// 2.1.4: the v4 copy's dot is `vpdpbusd`; the v3 copy has none.
TEST(XpuCpuPortable, theV4CopyTakesVnniAndTheV3CopyDoesNot) {
    auto base = freshDir("irdot");
    auto src = base / "probe";
    fs::create_directories(src);
    fs::create_directories(base / "build");
    std::ofstream(src / "P.cajeta") << kProbe;
    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Cpu);
    compiler.setCpu("portable");
    compiler.compile("probe.P.run", base.string(), (base / "build").string());
    std::string ir;
    for (auto& e : fs::recursive_directory_iterator(base / "build"))
        if (e.is_regular_file() && e.path().extension() == ".ll"
                && e.path().filename().string().find("__stdlib__") == std::string::npos) {
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            ir = ss.str();
        }
    fs::remove_all(base);
    ASSERT_FALSE(ir.empty());
    const std::string v4 = functionText(ir, "__cajeta_xpu_cpu_block.probe.P.dot.v4");
    const std::string v3 = functionText(ir, "__cajeta_xpu_cpu_block.probe.P.dot.v3");
    ASSERT_FALSE(v4.empty());
    ASSERT_FALSE(v3.empty());
    EXPECT_NE(v4.find("llvm.x86.avx512.vpdpbusd"), std::string::npos);
    EXPECT_EQ(v3.find("vpdpbusd"), std::string::npos);
}

// 2.1.1 (run half) and 2.1.3: the probe built once runs at every level the host
// reaches, reports the level it resolved and the wave that level built, and
// computes the same bits at each.
TEST(XpuCpuPortable, onePortableBinaryRunsAtEveryLevelWithTheSameBits) {
    auto base = freshDir("run");
    std::string log;
    const std::string exe = buildProbe(base, &log);
    ASSERT_FALSE(exe.empty()) << "the portable probe failed to build:\n" << log;
    const std::string plain = capture("\"" + exe + "\"");
    ASSERT_FALSE(plain.empty()) << "no output";
    const std::string host = field(plain, "level");
    const std::string sum = field(plain, "checksum");
    ASSERT_FALSE(sum.empty()) << plain;
    EXPECT_EQ(field(plain, "kernel"), host) << plain;
    std::vector<std::string> levels = {"v3", "v3-vnni", "v4", "v4-amx"};
    auto rank = [&](const std::string& l) { return (int) (std::find(levels.begin(), levels.end(), l) - levels.begin()); };
    ASSERT_LT(rank(host), 4) << "host level: " << host;
    for (const auto& lvl : levels) {
        const std::string out = capture("CAJETA_XPU_CPU_LEVEL=" + lvl + " \"" + exe + "\"");
        ASSERT_FALSE(out.empty()) << lvl;
        const std::string want = rank(lvl) <= rank(host) ? lvl : host;   // the override never raises
        EXPECT_EQ(field(out, "level"), want) << out;
        EXPECT_EQ(field(out, "kernel"), want) << out;
        EXPECT_EQ(field(out, "checksum"), sum) << "the " << lvl << " copy's bits differ: " << out;
        const std::string wave = field(out, "wave");
        if (want == "v3" || want == "v3-vnni") EXPECT_EQ(wave, "8") << out;
        else EXPECT_EQ(wave, "16") << out;
    }
    fs::remove_all(base);
}
