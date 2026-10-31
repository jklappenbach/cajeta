// cpu-isa-variants Unit 6: the AMX tier for int8 16x16 cooperative-matrix tiles on
// the cpu backend. There is no AMX host here, so this holds the IR's shape and the
// instruction selection through the toolchain's x86 backend (spec 5.4), never a run.
#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "../PortableEnv.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

const char* kGemm = R"CJ(
package amx;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.CooperativeMatrix;
public final class G {
    @Kernel
    public static void gemmI8(KernelBuffer<int32> c, KernelBuffer<int8> a,
                              KernelBuffer<int8> b, uint32 depth) {
        CooperativeMatrix<int32,16,16,2> mc;
        mc.splat(0);
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        ma.load(a, 0, 0, depth);
        mb.load(b, 0, 1, depth);
        mc.mma(ma, mb);
        mc.store(c, 0, 0, depth);
    }
    public static void run() { return; }
}
)CJ";

struct AmxEnv {
    explicit AmxEnv(const char* v) { if (v) setenv("CAJETA_XPU_CPU_AMX", v, 1); else unsetenv("CAJETA_XPU_CPU_AMX"); }
    ~AmxEnv() { unsetenv("CAJETA_XPU_CPU_AMX"); }
};

std::string irOfGemm(const char* tag) {
    auto base = fs::temp_directory_path() / ("cajeta-amx-" + std::string(tag) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "src" / "amx");
    fs::create_directories(base / "build");
    std::ofstream(base / "src" / "amx" / "G.cajeta") << kGemm;
    cajeta::Compiler compiler;
    compiler.setEmitMode(cajeta::EmitMode::IR);
    compiler.setXpuBackend(cajeta::XpuBackend::Cpu);
    compiler.setCpu("portable");
    compiler.setCpuVariants({"v4", "v4-amx"});
    compiler.compile("amx.G.run", (base / "src").string(), (base / "build").string());
    std::string ir;
    for (auto& e : fs::recursive_directory_iterator(base / "build"))
        if (e.is_regular_file() && e.path().extension() == ".ll"
                && e.path().filename().string().find("__stdlib__") == std::string::npos) {
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            ir += ss.str();
        }
    fs::remove_all(base);
    return ir;
}

// The text of one defined function, `define ... @name(` to its closing brace.
std::string functionText(const std::string& ir, const std::string& name) {
    size_t at = ir.find("@" + name + "(");
    while (at != std::string::npos) {
        size_t line = ir.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;
        if (ir.compare(line, 6, "define") == 0) {
            size_t end = ir.find("\n}\n", at);
            return ir.substr(line, end == std::string::npos ? std::string::npos : end + 3 - line);
        }
        at = ir.find("@" + name + "(", at + 1);
    }
    return "";
}

// The assembly the toolchain's x86 backend selects for `ir` at level v4-amx.
std::string assemblyOf(const std::string& ir) {
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> mod = llvm::parseAssemblyString(ir, err, ctx);
    if (!mod) { ADD_FAILURE() << err.getMessage().str(); return ""; }
    std::unique_ptr<llvm::TargetMachine> tm = cajeta::xpu::cpu::createCpuTargetMachine("v4-amx");
    if (!tm) { ADD_FAILURE() << "no v4-amx target machine"; return ""; }
    mod->setDataLayout(tm->createDataLayout());
    llvm::SmallString<1 << 16> asmText;
    llvm::raw_svector_ostream os(asmText);
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        ADD_FAILURE() << "the target cannot emit assembly"; return "";
    }
    pm.run(*mod);
    return std::string(asmText.str());
}

unsigned count(const std::string& s, const std::string& needle) {
    unsigned n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
}  // namespace

// 6.1.1: with the tier enabled, the v4-amx copy lowers the int8 tiles to the AMX
// intrinsics and the x86 backend selects the tile instructions; the v4 copy beside
// it keeps the portable tile.
TEST(XpuCpuAmxTier, theV4AmxCopyTakesTheTileIntrinsicsWhenEnabled) {
    AmxEnv on("1");
    const std::string ir = irOfGemm("on");
    ASSERT_FALSE(ir.empty());
    const std::string amx = functionText(ir, "__cajeta_xpu_cpu_block.amx.G.gemmI8.v4-amx");
    const std::string v4 = functionText(ir, "__cajeta_xpu_cpu_block.amx.G.gemmI8.v4");
    ASSERT_FALSE(amx.empty()) << "no v4-amx copy of the kernel";
    ASSERT_FALSE(v4.empty());
    EXPECT_NE(amx.find("llvm.x86.tileloadd64.internal"), std::string::npos) << amx;
    EXPECT_NE(amx.find("llvm.x86.tdpbssd.internal"), std::string::npos) << "int8 x int8 is the signed-signed dot: " << amx;
    EXPECT_NE(amx.find("llvm.x86.tilestored64.internal"), std::string::npos) << amx;
    EXPECT_NE(amx.find("llvm.x86.tilezero.internal"), std::string::npos) << "splat(0) is tilezero: " << amx;
    EXPECT_EQ(v4.find("llvm.x86.t"), std::string::npos) << "the v4 copy keeps the portable tile";
    const std::string asmText = assemblyOf(ir);
    ASSERT_FALSE(asmText.empty());
    EXPECT_GE(count(asmText, "tdpbssd"), 1u);
    EXPECT_GE(count(asmText, "tileloadd"), 2u) << "A and the VNNI-packed B";
    EXPECT_GE(count(asmText, "tilestored"), 1u);
    EXPECT_GE(count(asmText, "ldtilecfg"), 1u);
}

// 6.1.2: the tier is off unless CAJETA_XPU_CPU_AMX=1, so a v4-amx copy built by
// default carries no tile intrinsic. (That the resolver never picks v4-amx without
// the OS grant is XpuCpuLevel.theHostLevelFollowsCpuid.)
TEST(XpuCpuAmxTier, theTierIsOffByDefault) {
    AmxEnv off(nullptr);
    const std::string ir = irOfGemm("off");
    ASSERT_FALSE(ir.empty());
    const std::string amx = functionText(ir, "__cajeta_xpu_cpu_block.amx.G.gemmI8.v4-amx");
    ASSERT_FALSE(amx.empty());
    EXPECT_EQ(amx.find("llvm.x86.t"), std::string::npos) << amx;
}
