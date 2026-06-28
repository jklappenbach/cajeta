//
// CajetaXPU — vectorized device-staging probe (xpu-device-vectorized-staging U1).
// GPU-free ISA inspection: does a wide `vstore`/`vload` on a static `Shared<T>` (LDS)
// tile lower to a single 128-bit LDS op (ds_write_b128 / ds_read_b128), or scalarize
// to per-element ds_*_u16? A static Shared tile is already a registered buffer base
// (addrspace 3) and bufferElementPtr is addrspace-preserving, so the wide path SHOULD
// already form; the open question is whether the 2-byte element ABI alignment defeats
// the 128-bit selection. This probe DECIDES kernel-only vs an alignment codegen change
// (plan 1.3.2). Descriptive (records counts), one TEST per WMMA width: f16 N=8,
// int8 N=16, f32 N=4. (No GPU; same harness as XpuTorchConfigProbeTests.)
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// One thread stages a 128-bit chunk global -> register -> LDS and back: wide load
// from the global buffer `a` (vload<N>), wide store into the static Shared tile `s`
// (vstore), wide load back from LDS, wide store to global `out`. Indices are `tid*N`
// so they are provably multiples of N (the aligned-staging case). Parameterized on the
// element type and lane count so each WMMA width gets its own kernel.
std::string stagingSrc(const char* elemTy, const char* outTy, int lanes, int tileLen) {
    std::ostringstream s;
    s << "package test;\n"
         "import cajeta.xpu.KernelBuffer;\n"
         "import cajeta.xpu.KernelThread;\n"
         "import cajeta.xpu.Barrier;\n"
         "import cajeta.xpu.Shared;\n"
         "public class M {\n"
         "    @Kernel\n"
         "    public static void stage(KernelBuffer<" << outTy << "> out,\n"
         "                             KernelBuffer<" << elemTy << "> a, uint32 n) {\n"
         "        uint32 tid = KernelThread.x();\n"
         "        Shared<" << elemTy << "> s = shared " << elemTy << "[" << tileLen << "];\n"
         "        uint32 g = tid * " << lanes << ";\n"
         "        Vector<" << elemTy << "," << lanes << "> v = a.vload<" << lanes << ">(g);\n"
         "        s.vstore(g, v);\n"
         "        Barrier.workgroup();\n"
         "        Vector<" << elemTy << "," << lanes << "> w = s.vload<" << lanes << ">(g);\n"
         "        out.vstore(g, w);\n"
         "    }\n"
         "}\n";
    return s.str();
}

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vstage_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vstage_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

int parseAmdMeta(const std::string& isa, const std::string& key) {
    std::istringstream ss(isa);
    std::string line;
    std::string needle = "." + key + ":";
    while (std::getline(ss, line)) {
        auto p = line.find(needle);
        if (p == std::string::npos) continue;
        p += needle.size();
        while (p < line.size() && !std::isdigit((unsigned char) line[p])) ++p;
        if (p >= line.size()) return -1;
        return std::atoi(line.c_str() + p);
    }
    return -1;
}

std::string isaOf(const std::string& source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_vstage_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Probe the LDS feed + global side for one width. `wantLdsB128` asserts a single
// ds_*_b128 (f16/int8 at 128-bit); f32x4 instead selects paired ds_*_2addr_b32, which
// is still wide (no per-element scalarization). Every width must: lower cleanly, not
// spill, NOT scalarize to per-element ds_*_u16/u8, and load the global side wide
// (global_load_b128 on RDNA3.5 — the dwordx4 alias is CDNA).
void probe(const char* label, const std::string& src, bool wantLdsB128) {
    std::string isa = isaOf(src, "stage");
    ASSERT_FALSE(isa.empty()) << label << ": staging kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    int dsW128 = countOccurrences(isa, "ds_write_b128") + countOccurrences(isa, "ds_store_b128");
    int dsR128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    // Per-element scalarization signal (sub-32-bit LDS ops) — the U2 trigger.
    int dsScalar = countOccurrences(isa, "ds_write_b16") + countOccurrences(isa, "ds_store_b16")
                 + countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16")
                 + countOccurrences(isa, "ds_write_b8") + countOccurrences(isa, "ds_store_b8")
                 + countOccurrences(isa, "ds_read_u8") + countOccurrences(isa, "ds_load_u8");
    int glWide = countOccurrences(isa, "global_load_b128")
               + countOccurrences(isa, "global_load_dwordx4");
    std::cerr << "[vstage] " << label << ":\n"
              << "  vgpr_spill=" << spill
              << "  LDS b128 write=" << dsW128 << " read=" << dsR128
              << "  LDS scalar(u16/u8)=" << dsScalar
              << "  global wide=" << glWide << "\n";
    if (std::getenv("CAJETA_XPU_VSTAGE_DUMP")) {
        std::istringstream ls(isa);
        std::string line;
        while (std::getline(ls, line)) {
            if (line.find("ds_") != std::string::npos
                    || line.find("global_load") != std::string::npos
                    || line.find("global_store") != std::string::npos)
                std::cerr << "    | " << line << "\n";
        }
    }
    EXPECT_EQ(spill, 0) << label << ": staging probe should not spill";
    EXPECT_EQ(dsScalar, 0) << label << ": LDS feed must not scalarize to per-element ops";
    EXPECT_GT(glWide, 0) << label << ": global side should load wide (b128)";
    if (wantLdsB128) {
        EXPECT_GT(dsW128, 0) << label << ": LDS store should be b128";
        EXPECT_GT(dsR128, 0) << label << ": LDS load should be b128";
    }
}

} // namespace

// f16 N=8 (the torch-parity width): 8 x half = 128 bit -> ds_*_b128.
TEST(XpuVectorStagingProbeTests, f16Width8LdsWideFeed) {
    probe("f16 N=8", stagingSrc("float16", "float16", 8, 2048), true);
}

// int8 N=16: 16 x i8 = 128 bit -> ds_*_b128.
TEST(XpuVectorStagingProbeTests, int8Width16LdsWideFeed) {
    probe("int8 N=16", stagingSrc("int8", "int8", 16, 4096), true);
}

// f32 N=4: 4 x float = 128 bit. Global loads b128; LDS selects paired ds_*_2addr_b32
// (wide, not scalarized) rather than a single b128 — so don't require b128 here.
TEST(XpuVectorStagingProbeTests, f32Width4LdsWideFeed) {
    probe("f32 N=4", stagingSrc("float32", "float32", 4, 1024), false);
}
