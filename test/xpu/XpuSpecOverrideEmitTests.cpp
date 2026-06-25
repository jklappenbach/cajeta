//
// Cajeta XPU — host spec-constant override, device-baking backends (Phase C).
//
// NVPTX + AMDGPU bake their kernels at compile time (no pipeline-time binding),
// so they honor a host `spec:[...]` override by reading it from a pair of
// constant-memory globals the runtime sets per launch:
//   result = (slot u< __cajeta_xpu_spec_count) ? __cajeta_xpu_spec_values[slot]
//                                               : <compile-time default>
// SAFE BY DEFAULT: the globals are zero-initialized, so if the runtime never
// sets them the read returns the baked default (no regression).
//
// These are EMIT gates — they prove the override read is wired into the device
// code (the constant globals are referenced, not just a baked literal). ON-DEVICE
// execution is deferred: AMD in-process JIT is comgr-blocked and there is no
// NVIDIA hardware here (see project memory), so the per-launch constant copy in
// the runtime is unverified-on-device pending that hardware.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::EmitMode;
using cajeta::XpuBackend;
using cajeta::XpuEmit;

namespace {

namespace fs = std::filesystem;

// A spec-using kernel: out[i] = Spec.geti(0, 7) (+ a float slot for the f32 path).
const char* kSpecKernelSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class SC {\n"
    "    @Kernel\n"
    "    public static void fill(KernelBuffer<int32> out, KernelBuffer<float32> outf,\n"
    "                            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i] = Spec.geti(0, 7);\n"
    "            outf[i] = Spec.getf(1, 2.5f);\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& src,
                                     const std::string& fqClass) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_specemit_" + std::to_string(rng()));
    fs::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClass.size(); ++i)
        if (i == fqClass.size() || fqClass[i] == '.') {
            rel /= fqClass.substr(start, i - start);
            start = i + 1;
        }
    rel += ".cajeta";
    auto full = base / rel;
    fs::create_directories(full.parent_path());
    std::ofstream(full) << src;
    auto archive = fs::temp_directory_path() / ("cajeta_xpu_specemit_a_" + std::to_string(rng()));
    fs::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// AMD: compile fill to gfx1151 .isa and return the ISA text.
std::string amdIsaForFill() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_specamd_" + std::to_string(rng()));
    auto src = base / "src"; auto build = base / "build";
    fs::create_directories(src / "test"); fs::create_directories(build);
    std::ofstream(src / "test" / "SC.cajeta") << kSpecKernelSrc;
    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Amdgpu);
    compiler.setXpuEmit(XpuEmit::Isa);
    compiler.setXpuArch("gfx1151");
    compiler.compile("test.SC.fill", src.string(), build.string());
    std::string isa;
    if (fs::exists(build))
        for (auto& e : fs::recursive_directory_iterator(build))
            if (e.is_regular_file() && e.path().extension() == ".isa") {
                std::ifstream in(e.path(), std::ios::binary);
                std::ostringstream ss; ss << in.rdbuf();
                if (ss.str().find(".amdhsa_kernel fill") != std::string::npos) {
                    isa = ss.str(); break;
                }
            }
    fs::remove_all(base);
    return isa;
}

} // namespace

// NVPTX: the override read is wired into the PTX — both constant globals are
// referenced (so the runtime can set them) and a select chooses override-vs-default.
TEST(XpuSpecOverrideEmitTests, nvptxReadsOverrideFromConstantMemory) {
    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    Compiler compiler;
    auto module = compileForInspection(compiler, kSpecKernelSrc, "test.SC");
    auto klass = module->getStructures()["test.SC"];
    ASSERT_NE(klass, nullptr);
    auto fill = findMethod(klass, "fill");
    ASSERT_NE(fill, nullptr);

    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_spec_nvptx", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(fill, deviceModule), nullptr);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find("__cajeta_xpu_spec_count"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("__cajeta_xpu_spec_values"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("ld.const"), std::string::npos) << ptx;   // read from constant mem
    EXPECT_NE(ptx.find("selp"), std::string::npos) << ptx;       // override ? value : default
}

// AMD/gfx1151: same — the override read rides the constant globals in the .isa.
TEST(XpuSpecOverrideEmitTests, amdgpuReadsOverrideFromConstantMemory) {
    std::string isa = amdIsaForFill();
    ASSERT_FALSE(isa.empty()) << "no gfx1151 .isa for fill";
    EXPECT_NE(isa.find("__cajeta_xpu_spec_count"), std::string::npos) << isa;
    EXPECT_NE(isa.find("__cajeta_xpu_spec_values"), std::string::npos) << isa;
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;
}
