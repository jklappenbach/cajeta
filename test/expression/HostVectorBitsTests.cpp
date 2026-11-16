//
// host-simd Unit 3 (specs/host-vector-width-spec.md): Cajeta.hostVectorBits()
// answers the fixed-width vector register width the code is compiled for. It
// resolves when a module is emitted as code; under the JIT that is the host, so
// the answer is what a direct resolution for the host gives, and no call is left behind.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Optimizer.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
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

// What resolveHostVectorBits answers for a function built for `cpu` ("" for this
// machine's CPU and features): a module with one call of the query, resolved.
int32_t resolvedFor(const std::string& cpu) {
    llvm::Triple triple(cpu.empty() ? llvm::sys::getProcessTriple() : "x86_64-unknown-linux-gnu");
    std::string err;
    const llvm::Target* t = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!t) return -1;
    std::string features;
    std::string name = cpu;
    if (cpu.empty()) {
        llvm::SubtargetFeatures feats;
        for (auto& kv : llvm::sys::getHostCPUFeatures()) feats.AddFeature(kv.getKey(), kv.getValue());
        features = feats.getString();
        name = llvm::sys::getHostCPUName().str();
    }
    std::unique_ptr<llvm::TargetMachine> tm(t->createTargetMachine(
        triple, name, features, llvm::TargetOptions(), std::nullopt));
    if (!tm) return -2;
    llvm::LLVMContext ctx;
    llvm::Module m("probe", ctx);
    m.setTargetTriple(triple);
    m.setDataLayout(tm->createDataLayout());
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* q = llvm::Function::Create(llvm::FunctionType::get(i32, false),
        llvm::Function::ExternalLinkage, cajeta::kHostVectorBitsSymbol, m);
    auto* f = llvm::Function::Create(llvm::FunctionType::get(i32, false),
        llvm::Function::ExternalLinkage, "run", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", f));
    b.CreateRet(b.CreateCall(q, {}));
    cajeta::resolveHostVectorBits(m, tm.get());
    auto* ret = llvm::cast<llvm::ReturnInst>(f->getEntryBlock().getTerminator());
    auto* c = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
    return c ? (int32_t) c->getSExtValue() : -3;
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
    int32_t want = resolvedFor("");
    ASSERT_GE(want, 128);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(want, run());
    EXPECT_EQ(want, cajeta::hostVectorBitsOfHost());
    EXPECT_EQ(jit->getModuleIr().find("call i32 @__cajeta_host_vector_bits"), std::string::npos);
}

// 2.1a — the answer is the widest vector the build holds in registers, which is
// what explicit Vector<T,N> code is lowered to. x86-64-v4 reads 512: its generic
// tuning prefers 256-bit vectors for the auto-vectorizer, but a 16-lane float32
// fma there is one zmm vfmadd (measured with the fork's llc, 2026-11-15).
TEST(HostVectorBitsTests, theAnswerIsTheWidestRegisterTheBuildHolds) {
    CajetaJit::compile(kSrc, "test.D");   // registers the targets
    EXPECT_EQ(512, resolvedFor("x86-64-v4"));
    EXPECT_EQ(512, resolvedFor("znver5"));
    EXPECT_EQ(512, resolvedFor("skylake-avx512"));
    EXPECT_EQ(256, resolvedFor("x86-64-v3"));
    EXPECT_EQ(128, resolvedFor("x86-64-v2"));
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
