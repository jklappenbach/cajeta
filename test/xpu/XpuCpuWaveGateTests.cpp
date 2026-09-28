//
// xpu-kernel-adaptor 4.2.1.2 — the left-scalar gate has to see a scalar wave
// call INSIDE the vector loop for what it is.
//
// MEASURED HOLE. `waveOpLeftScalar` asked one question of a scalar wave stub
// call: is the work-item loop it sits in marked `llvm.loop.isvectorized`?
// LoopVectorize marks BOTH loops it leaves behind, the vector loop and the
// scalar remainder, so a call that the vectorizer scalarized (a
// `pred.call.if` block, one scalar call per lane under a mask, or a
// "uniform" call replicated once per vector iteration) sat inside a loop
// that answered yes, and the gate passed it. blockReduce ran eight of them
// and computed the width-1 identity, 3968 for 32640 (measured 2026-09-25).
//
// These tests hand the gate IR shaped the way LoopVectorize leaves it, so the
// rule is pinned on the shape rather than on one kernel that happens to
// provoke it: a scalar stub call in the vector loop is LEFT SCALAR; in the
// remainder loop it is the epilogue and passes; under an unwidened work-item
// loop it is left scalar as before.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <memory>
#include <string>

namespace {

struct Parsed {
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::Function* fn = nullptr;
};

std::unique_ptr<Parsed> parse(const std::string& ir) {
    auto p = std::make_unique<Parsed>();
    llvm::SMDiagnostic err;
    p->mod = llvm::parseAssemblyString(ir, err, p->ctx);
    EXPECT_NE(p->mod, nullptr) << err.getMessage().str();
    if (p->mod) p->fn = p->mod->getFunction("k");
    EXPECT_NE(p->fn, nullptr);
    return p;
}

const char* kLoopMd = R"LL(
!0 = distinct !{!0, !1, !2}
!1 = !{!"llvm.loop.isvectorized", i32 1}
!2 = !{!"cajeta.xpu.wi"}
!3 = distinct !{!3, !2}
)LL";

// The vector loop LoopVectorize builds, with the wave stub scalarized under a
// per-lane predicate: exactly the `pred.call.if` shape the remark leaves.
std::string predicatedInVectorLoop() {
    return std::string(R"LL(
declare float @__cajeta_xpu_wave_reduce_sum_f32(float)
define void @k(ptr %out, i32 %n) {
entry:
  br label %vector.body
vector.body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %pred.call.continue ]
  %c = icmp eq i32 %iv, 0
  br i1 %c, label %pred.call.if, label %pred.call.continue
pred.call.if:
  %r = call float @__cajeta_xpu_wave_reduce_sum_f32(float 1.0)
  store float %r, ptr %out
  br label %pred.call.continue
pred.call.continue:
  %iv.next = add i32 %iv, 8
  %done = icmp ult i32 %iv.next, %n
  br i1 %done, label %vector.body, label %exit, !llvm.loop !0
exit:
  ret void
}
)LL") + kLoopMd;
}

// The same call replicated unpredicated in the vector loop (a "uniform" call
// LoopVectorize chose not to widen): still one scalar call, still width 1.
std::string replicatedInVectorLoop() {
    return std::string(R"LL(
declare float @__cajeta_xpu_wave_reduce_sum_f32(float)
define void @k(ptr %out, i32 %n) {
entry:
  br label %vector.body
vector.body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %vector.body ]
  %r = call float @__cajeta_xpu_wave_reduce_sum_f32(float 1.0)
  store float %r, ptr %out
  %iv.next = add i32 %iv, 8
  %done = icmp ult i32 %iv.next, %n
  br i1 %done, label %vector.body, label %exit, !llvm.loop !0
exit:
  ret void
}
)LL") + kLoopMd;
}

// The scalar remainder loop: the original work-item loop, marked vectorized
// by LoopVectorize so it is not widened twice, holding the original scalar
// call. It runs the tail iterations only, and passes.
std::string inRemainderLoop() {
    return std::string(R"LL(
declare float @__cajeta_xpu_wave_reduce_sum_f32(float)
define void @k(ptr %out, i32 %n) {
entry:
  br label %wi.head
wi.head:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %wi.head ]
  %r = call float @__cajeta_xpu_wave_reduce_sum_f32(float 1.0)
  store float %r, ptr %out
  %iv.next = add i32 %iv, 1
  %done = icmp ult i32 %iv.next, %n
  br i1 %done, label %wi.head, label %exit, !llvm.loop !0
exit:
  ret void
}
)LL") + kLoopMd;
}

// A work-item loop the vectorizer never widened: left scalar, as before.
std::string inUnwidenedLoop() {
    return std::string(R"LL(
declare float @__cajeta_xpu_wave_reduce_sum_f32(float)
define void @k(ptr %out, i32 %n) {
entry:
  br label %wi.head
wi.head:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %wi.head ]
  %r = call float @__cajeta_xpu_wave_reduce_sum_f32(float 1.0)
  store float %r, ptr %out
  %iv.next = add i32 %iv, 1
  %done = icmp ult i32 %iv.next, %n
  br i1 %done, label %wi.head, label %exit, !llvm.loop !3
exit:
  ret void
}
)LL") + kLoopMd;
}

TEST(XpuCpuWaveGate, aPredicatedScalarCallInsideTheVectorLoopIsLeftScalar) {
    auto p = parse(predicatedInVectorLoop());
    ASSERT_NE(p->fn, nullptr);
    std::string which;
    EXPECT_TRUE(cajeta::xpu::cpu::waveOpLeftScalar(*p->fn, &which))
        << "a pred.call.if scalar stub inside vector.body is one scalar call "
           "per lane, the width-1 identity, whatever the loop metadata says";
    EXPECT_EQ(which, "__cajeta_xpu_wave_reduce_sum_f32");
}

TEST(XpuCpuWaveGate, aReplicatedScalarCallInsideTheVectorLoopIsLeftScalar) {
    auto p = parse(replicatedInVectorLoop());
    ASSERT_NE(p->fn, nullptr);
    std::string which;
    EXPECT_TRUE(cajeta::xpu::cpu::waveOpLeftScalar(*p->fn, &which))
        << "an unpredicated scalar stub inside vector.body was replicated, "
           "not widened; it runs at width 1";
}

TEST(XpuCpuWaveGate, theScalarRemainderLoopIsTheEpilogueAndPasses) {
    auto p = parse(inRemainderLoop());
    ASSERT_NE(p->fn, nullptr);
    std::string which;
    EXPECT_FALSE(cajeta::xpu::cpu::waveOpLeftScalar(*p->fn, &which))
        << "the vectorizer's own scalar remainder keeps the scalar call and "
           "must not be refused: " << which;
}

TEST(XpuCpuWaveGate, anUnwidenedWorkItemLoopIsStillLeftScalar) {
    auto p = parse(inUnwidenedLoop());
    ASSERT_NE(p->fn, nullptr);
    std::string which;
    EXPECT_TRUE(cajeta::xpu::cpu::waveOpLeftScalar(*p->fn, &which));
}

}  // namespace
