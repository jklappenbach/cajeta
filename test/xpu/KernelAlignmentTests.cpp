//
// KernelAlignmentTests — the shared alignment proof (kernel-load-alignment
// plan Unit 1, spec 2.2-2.6). One proof answers for NVPTX, AMDGPU and
// Vulkan, so its algebra is pinned here on IR, backend-free: the NVPTX
// cases it moved with (NvptxWordFormTests keeps the PTX side), the merges
// and masks the llm's hand-aligned offsets need, and the 16-byte cap.
// Each "fires" case has a "does not fire" beside it.
//
#include <gtest/gtest.h>

#include "cajeta/xpu/lowering/AlignmentProof.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <memory>
#include <string>

namespace {

// The proven alignment of the first load in `@f`, whose address is derived
// from argument 0, with the base aligned to 16 bytes (the launch contract).
uint64_t provenOfFirstLoad(const std::string& body) {
    static llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::string ir = "target datalayout = \"e-i64:64-v16:16-v32:32-n16:32:64\"\n" + body;
    std::unique_ptr<llvm::Module> m = llvm::parseAssemblyString(ir, err, ctx);
    if (!m) {
        ADD_FAILURE() << "IR did not parse: " << err.getMessage().str();
        return 0;
    }
    llvm::Function* f = m->getFunction("f");
    for (llvm::BasicBlock& bb : *f)
        for (llvm::Instruction& i : bb)
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&i))
                return cajeta::xpu::provenAlignment(ld->getPointerOperand(), f->getArg(0),
                                                    16, m->getDataLayout());
    ADD_FAILURE() << "no load in @f";
    return 0;
}

}  // namespace

// 1.1.1: a product, a sum of aligned terms (the Q4_K row offset's shape).
TEST(KernelAlignment, aProductAndASumOfAlignedTermsAreProven) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = mul i64 %i, 32
  %o2 = add i64 %o, 16
  %p = getelementptr i8, ptr %b, i64 %o2
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.1, does not fire: an index nothing proves stays at one byte (spec 2.2).
TEST(KernelAlignment, aRuntimeOffsetProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %off) {
  %o = add i64 %i, %off
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.1: a loop-carried offset is proven from its start and its step.
TEST(KernelAlignment, aLoopCarriedOffsetIsProvenFromItsStartAndStep) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %n) {
entry:
  %s = mul i64 %i, 64
  br label %loop
loop:
  %o = phi i64 [ %s, %entry ], [ %o2, %loop ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  %o2 = add i64 %o, 32
  %c = icmp ult i64 %o2, %n
  br i1 %c, label %loop, label %done
done:
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.1, does not fire: an aligned step from an unproven start proves nothing.
TEST(KernelAlignment, aLoopCarriedOffsetFromAnUnprovenStartProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %start, i64 %n) {
entry:
  br label %loop
loop:
  %o = phi i64 [ %start, %entry ], [ %o2, %loop ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  %o2 = add i64 %o, 32
  %c = icmp ult i64 %o2, %n
  br i1 %c, label %loop, label %done
done:
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.2: a select of a 16- and a 4-aligned value proves 4 (spec 2.3).
TEST(KernelAlignment, aSelectProvesTheSmallerArm) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
  %a = mul i64 %i, 16
  %w = mul i64 %j, 4
  %o = select i1 %c, i64 %a, i64 %w
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 4u);
}

// 1.1.2: an if-merge (phi) of a 16- and a 4-aligned value proves 4.
TEST(KernelAlignment, anIfMergeProvesTheSmallerArm) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
entry:
  br i1 %c, label %t, label %e
t:
  %a = mul i64 %i, 16
  br label %m
e:
  %w = mul i64 %j, 4
  br label %m
m:
  %o = phi i64 [ %a, %t ], [ %w, %e ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 4u);
}

// 1.1.2, does not fire: a merge with an unproven arm proves nothing.
TEST(KernelAlignment, aMergeWithAnUnprovenArmProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
  %a = mul i64 %i, 16
  %o = select i1 %c, i64 %a, i64 %j
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.3: a hand mask (the llm's a16) proves its alignment whatever the index
// (spec 2.4), so the existing masks stay correct.
TEST(KernelAlignment, aMaskOfMinusSixteenProvesSixteen) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = and i64 %i, -16
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.3, does not fire: a low-bit mask proves nothing.
TEST(KernelAlignment, aMaskOfFifteenProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = and i64 %i, 15
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.4: a product by 64 is credited 16, the widest access (spec 2.6).
TEST(KernelAlignment, aProvenAlignmentAboveSixteenIsSixteen) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = mul i64 %i, 64
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}
