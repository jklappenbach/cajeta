//
// runGuardedCodegen (xpu-kernel-adaptor 3.2.5): an LLVM fatal error during one
// kernel's codegen comes back as a false return and its reason, so the build
// names that kernel and goes on. Before, LLVM printed "LLVM ERROR" and exited,
// and one kernel took a whole backend's build down with it.
//
#include "gtest/gtest.h"

#include "cajeta/xpu/core/CodegenGuard.h"

#include "llvm/Support/ErrorHandling.h"

#include <string>

// The fatal is raised in-process: this test surviving it is the claim.
TEST(CodegenGuard, anLlvmFatalErrorIsReturnedWithItsReason) {
    std::string why;
    bool ok = cajeta::xpu::runGuardedCodegen(
        [] { llvm::report_fatal_error("unable to legalize a probe instruction"); }, why);
    EXPECT_FALSE(ok);
    EXPECT_EQ(why, "unable to legalize a probe instruction");
}

TEST(CodegenGuard, aCleanRunReturnsTrueAndNoReason) {
    std::string why;
    int ran = 0;
    EXPECT_TRUE(cajeta::xpu::runGuardedCodegen([&] { ran = 1; }, why));
    EXPECT_EQ(ran, 1);
    EXPECT_TRUE(why.empty());
}

// The handler is scoped: a second guarded run after a caught fatal still catches.
TEST(CodegenGuard, theGuardCatchesAgainAfterAFatal) {
    std::string first, second;
    cajeta::xpu::runGuardedCodegen([] { llvm::report_fatal_error("first"); }, first);
    EXPECT_FALSE(cajeta::xpu::runGuardedCodegen(
        [] { llvm::report_fatal_error("second"); }, second));
    EXPECT_EQ(second, "second");
}
