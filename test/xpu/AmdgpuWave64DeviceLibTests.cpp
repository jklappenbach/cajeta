//
// datacenter-gpus Unit 1 (specs/datacenter-gpus-spec.md §2.1): a wave-64 arch is
// first-class on amdgpu. The backend linked oclc_wavefrontsize64_off for every arch,
// which is wrong on CDNA (gfx9xx), whose waves are 64 wide: device-library code that
// reads __oclc_wavefrontsize64 then takes its wave-32 arm. And DeviceProfile knew
// only RDNA parts.
//
// The ROCm directory here is a fake one, written as bitcode by the test, so it runs
// on a box with no ROCm: ocml.bc's one function reads __oclc_wavefrontsize64 the way
// the real library does, and the two control files set it to 1 and 0.
//

#include <gtest/gtest.h>
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/core/DeviceProfile.h"
#include "../PortableEnv.h"   // setenv/unsetenv and cajeta_getpid on MinGW

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace cajeta::xpu;

namespace {

void writeBitcode(const fs::path& p, const std::function<void(llvm::Module&)>& fill) {
    llvm::LLVMContext ctx;
    llvm::Module m("lib", ctx);
    m.setTargetTriple(llvm::Triple(amd::kAmdgpuTriple));
    fill(m);
    std::error_code ec;
    llvm::raw_fd_ostream out(p.string(), ec);
    ASSERT_FALSE(ec) << p;
    llvm::WriteBitcodeToFile(m, out);
}

void controlGlobal(llvm::Module& m, const char* name, unsigned bits, uint64_t v) {
    auto* t = llvm::IntegerType::get(m.getContext(), bits);
    new llvm::GlobalVariable(m, t, true, llvm::GlobalValue::LinkOnceODRLinkage,
                             llvm::ConstantInt::get(t, v), name);
}

// A ROCm root with the bitcode the backend links for a kernel calling Math.sin.
struct FakeRocm {
    fs::path root;
    std::string oldPath;
    bool hadOld = false;
    FakeRocm() {
        root = fs::temp_directory_path() / ("cajeta-wave64-" + std::to_string(cajeta_getpid()));
        fs::remove_all(root);
        fs::path bc = root / "amdgcn" / "bitcode";
        fs::create_directories(bc);
        writeBitcode(bc / "ockl.bc", [](llvm::Module&) {});
        writeBitcode(bc / "ocml.bc", [](llvm::Module& m) {
            auto& ctx = m.getContext();
            auto* i8 = llvm::Type::getInt8Ty(ctx);
            auto* f32 = llvm::Type::getFloatTy(ctx);
            auto* wave = new llvm::GlobalVariable(m, i8, true, llvm::GlobalValue::ExternalLinkage,
                                                  nullptr, "__oclc_wavefrontsize64");
            auto* fn = llvm::Function::Create(llvm::FunctionType::get(f32, {f32}, false),
                                              llvm::Function::ExternalLinkage, "__ocml_sin_f32", m);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "e", fn));
            llvm::Value* w = b.CreateLoad(i8, wave);
            b.CreateRet(b.CreateFAdd(fn->getArg(0), b.CreateUIToFP(w, f32)));
        });
        writeBitcode(bc / "oclc_wavefrontsize64_on.bc",
                     [](llvm::Module& m) { controlGlobal(m, "__oclc_wavefrontsize64", 8, 1); });
        writeBitcode(bc / "oclc_wavefrontsize64_off.bc",
                     [](llvm::Module& m) { controlGlobal(m, "__oclc_wavefrontsize64", 8, 0); });
        for (const char* isa : {"942", "1151"}) {
            writeBitcode(bc / (std::string("oclc_isa_version_") + isa + ".bc"),
                         [](llvm::Module& m) { controlGlobal(m, "__oclc_ISA_version", 32, 1); });
        }
        if (const char* o = std::getenv("ROCM_PATH")) { oldPath = o; hadOld = true; }
        ::setenv("ROCM_PATH", root.string().c_str(), 1);
    }
    ~FakeRocm() {
        if (hadOld) ::setenv("ROCM_PATH", oldPath.c_str(), 1); else ::unsetenv("ROCM_PATH");
        fs::remove_all(root);
    }
};

// The __oclc_wavefrontsize64 a kernel calling __ocml_sin_f32 links for `arch`, or -1.
int64_t linkedWave64Flag(const std::string& arch) {
    auto tm = amd::createAmdgpuTargetMachine(arch);
    if (!tm) return -2;
    llvm::LLVMContext ctx;
    llvm::Module m("kernel", ctx);
    amd::configureDeviceModule(m, *tm);
    auto* f32 = llvm::Type::getFloatTy(ctx);
    auto* sinFn = llvm::Function::Create(llvm::FunctionType::get(f32, {f32}, false),
                                         llvm::Function::ExternalLinkage, "__ocml_sin_f32", m);
    auto* k = llvm::Function::Create(llvm::FunctionType::get(f32, {f32}, false),
                                     llvm::Function::ExternalLinkage, "k", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "e", k));
    b.CreateRet(b.CreateCall(sinFn, {k->getArg(0)}));
    amd::linkAmdDeviceLibsIfNeeded(m, *tm);
    auto* g = m.getGlobalVariable("__oclc_wavefrontsize64");
    if (!g || !g->hasInitializer()) return -1;
    auto* c = llvm::dyn_cast<llvm::ConstantInt>(g->getInitializer());
    return c ? (int64_t) c->getZExtValue() : -1;
}

} // namespace

// 1.1.1: CDNA links the wave-64 control, RDNA keeps wave 32's.
TEST(AmdgpuWave64DeviceLib, aCdnaArchLinksTheWave64Control) {
    FakeRocm r;
    EXPECT_EQ(1, linkedWave64Flag("gfx942"));
    EXPECT_EQ(0, linkedWave64Flag("gfx1151"));
}

// 1.1.1: the arch's native wave width, which the control follows.
TEST(AmdgpuWave64DeviceLib, theArchsNativeWaveWidth) {
    EXPECT_EQ(64u, amd::nativeWaveWidth("gfx942"));
    EXPECT_EQ(64u, amd::nativeWaveWidth("gfx90a"));
    EXPECT_EQ(64u, amd::nativeWaveWidth("gfx950"));
    EXPECT_EQ(32u, amd::nativeWaveWidth("gfx1151"));
    EXPECT_EQ(32u, amd::nativeWaveWidth("gfx1100"));
    EXPECT_EQ(32u, amd::nativeWaveWidth("gfx1201"));
}

// 1.1.2: DeviceProfile knows gfx942 (MI300X: wave 64, 64 KB of LDS a CU and a
// workgroup, 1024 threads, 32 waves a CU, a CU per multiprocessor), and an arch it
// has no facts for is refused rather than given another part's numbers.
TEST(AmdgpuWave64DeviceLib, deviceProfileKnowsGfx942) {
    DeviceModel m;
    ASSERT_TRUE(lookupArch("gfx942", m));
    EXPECT_EQ(64u, m.waveSize);
    EXPECT_EQ(1024u, m.maxThreadsPerBlock);
    EXPECT_EQ(65536u, m.ldsBytesPerMP);
    EXPECT_EQ(65536u, m.ldsBytesPerBlockArch);
    EXPECT_EQ(32u, m.maxWavesPerMP);
    EXPECT_EQ(1u, m.cuPerMultiprocessor);
    DeviceModel u;
    EXPECT_FALSE(lookupArch("gfx950", u));
}
