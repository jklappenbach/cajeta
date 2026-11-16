//
// host-simd Unit 3 (specs/host-vector-width-spec.md): Cajeta.hostVectorBits()
// answers the fixed-width vector register width the code is compiled for. It
// resolves when a module is emitted as code; under the JIT that is the host, so
// the answer equals the host TargetTransformInfo's, and no call is left behind.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/TargetParser/SubtargetFeature.h>

#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// The host's answer, asked of LLVM directly: what a function compiled for this
// machine's CPU and features has as fixed-width vector registers.
unsigned hostTtiBits() {
    llvm::Triple triple(llvm::sys::getProcessTriple());
    std::string err;
    const llvm::Target* t = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!t) return 0;
    llvm::SubtargetFeatures feats;
    for (auto& kv : llvm::sys::getHostCPUFeatures()) feats.AddFeature(kv.getKey(), kv.getValue());
    std::unique_ptr<llvm::TargetMachine> tm(t->createTargetMachine(
        triple, llvm::sys::getHostCPUName(), feats.getString(),
        llvm::TargetOptions(), std::nullopt));
    if (!tm) return 0;
    llvm::LLVMContext ctx;
    llvm::Module m("probe", ctx);
    m.setDataLayout(tm->createDataLayout());
    auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* f = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "probe", m);
    llvm::TargetTransformInfo tti = tm->getTargetTransformInfo(*f);
    return (unsigned) tti.getRegisterBitWidth(
        llvm::TargetTransformInfo::RGK_FixedWidthVector).getFixedValue();
}

const char* kSrc =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        return Cajeta.hostVectorBits();\n"
    "    }\n"
    "}\n";

} // namespace

// 3.1.1 — under the JIT the answer is the host's, and it resolved to a constant.
TEST(HostVectorBitsTests, theJitAnswersTheHostsWidth) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(kSrc, "test.D", opts);
    ASSERT_NE(jit, nullptr);
    // asked after the compile, which registers the native target
    unsigned want = hostTtiBits();
    ASSERT_GE(want, 128u);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ((int32_t) want, run());
    EXPECT_EQ(jit->getModuleIr().find("call i32 @__cajeta_host_vector_bits"), std::string::npos);
}

// 3.1.1 — the query takes no arguments.
TEST(HostVectorBitsTests, aQueryWithArgumentsIsRefused) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() { return Cajeta.hostVectorBits(1); }\n"
        "}\n";
    std::string code;
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        code = e.getErrorId();
    }
    EXPECT_EQ("CAJETA_ERROR_HOST_VECTOR_BITS_ARGS", code);
}
