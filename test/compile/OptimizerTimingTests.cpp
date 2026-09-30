//
// CAJETA_TIME_PASSES: where a compile spends its time, per LLVM pass.
//
// The cajeta-llm cpu test binary takes about an hour to compile, and the only
// way to say which pass is responsible was to attach gdb to the compiler,
// which this box's ptrace scope refuses to a sibling process. LLVM already
// keeps per-pass timers (`-time-passes` in opt and llc); cajeta has no LLVM
// option passthrough, so the switch is an environment variable read by the
// optimizer and the codegen: set, every pipeline run prints LLVM's "Pass
// execution timing report" to stderr; unset, nothing changes.
//

#include <gtest/gtest.h>

#include "cajeta/compile/Optimizer.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "PortableEnv.h"

#include <cstdlib>
#include <memory>
#include <string>

namespace {

std::unique_ptr<llvm::Module> smallModule(llvm::LLVMContext& ctx) {
    auto m = std::make_unique<llvm::Module>("timing", ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(i32, {i32}, false),
                                      llvm::Function::ExternalLinkage, "twice", m.get());
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    b.CreateRet(b.CreateAdd(fn->getArg(0), fn->getArg(0)));
    return m;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

const char* kReport = "Pass execution timing report";

} // namespace

TEST(OptimizerTimingTests, theSwitchPrintsThePipelineTimingReport) {
    ScopedEnv env("CAJETA_TIME_PASSES", "1");
    llvm::LLVMContext ctx;
    auto m = smallModule(ctx);
    testing::internal::CaptureStderr();
    cajeta::optimizeModule(*m, /*tm=*/nullptr, cajeta::OptLevel::O2);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find(kReport), std::string::npos) << err;
}

TEST(OptimizerTimingTests, withoutTheSwitchThePipelineIsSilent) {
    unsetenv("CAJETA_TIME_PASSES");
    llvm::LLVMContext ctx;
    auto m = smallModule(ctx);
    testing::internal::CaptureStderr();
    cajeta::optimizeModule(*m, /*tm=*/nullptr, cajeta::OptLevel::O2);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find(kReport), std::string::npos) << err;
}
