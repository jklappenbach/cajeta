// CPU kernel registration pass — see header. Each @Kernel is lowered into a
// fresh module, linked in under a decorated symbol, and registered by a ctor.

#include "CpuRegistration.h"
#include <llvm/IR/DiagnosticInfo.h>
#include "CpuKernelLowering.h"
#include "../lowering/KernelLowering.h"   // collectKernelParamInfo / KernelParamInfo
#include "CpuBackend.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta_xpu_abi.h"   // CAJETA_XPU_ABI_VERSION for the manifest identity
#include "CpuBarrierFission.h"
#include "cajeta/compile/Optimizer.h"

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"
#include <cctype>
#include <map>

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdlib>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// Debug seam: CAJETA_XPU_CPU_NO_VECTORIZE skips the work-item LoopVectorize pass.
bool cpuVectorizeDisabled() {
    return std::getenv("CAJETA_XPU_CPU_NO_VECTORIZE") != nullptr;
}

// --- Increment 5C: wave-op SIMD via the Vector Function ABI ----------------
// A wave op lowers to a scalar `__cajeta_xpu_wave_*` call; these give it a
// SIMD variant and force the work-item loop to the host's width W.

// The host's native i32 vector width, 0 without a TM. This is the wave width.
unsigned cpuVectorWidthI32(llvm::TargetMachine* tm, llvm::Function& f) {
    // Debug seam (the 5C flat-wave spike): force a cooperative wave to a fixed
    // width regardless of the kernel, to prove LoopVectorize takes VF=N at 2x
    // the register width with width-N wave-op variants.
    if (const char* e = std::getenv("CAJETA_XPU_CPU_WAVE_WIDTH")) {
        unsigned w = (unsigned) std::strtoul(e, nullptr, 10);
        if (w >= 2) return w;
    }
    // Per-kernel width: a DISTRIBUTED cooperative-matrix kernel pins the wave
    // width it was laid out for (prepareDistributedCoopMatrix, waveW == Cols) as
    // a `cajeta.xpu.coop-wavew` attribute on the kernel. Force the work-item loop
    // to that exact width rather than the host's. The marker is found in one of
    // two places: on `f` itself (the barrier-fission path clones the body into
    // the wrapper and copies the marker across before erasing the kernel), or on
    // the kernel `f` still calls (the plain single-loop path). Check both.
    auto readMarker = [](llvm::Function& g) -> unsigned {
        if (!g.hasFnAttribute("cajeta.xpu.coop-wavew")) return 0;
        return (unsigned) std::strtoul(
            g.getFnAttribute("cajeta.xpu.coop-wavew")
                .getValueAsString().str().c_str(),
            nullptr, 10);
    };
    if (unsigned w = readMarker(f); w >= 2) return w;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = call->getCalledFunction())
                    if (unsigned w = readMarker(*cf); w >= 2) return w;
    if (!tm) return 0;
    llvm::TargetTransformInfo tti = tm->getTargetTransformInfo(f);
    llvm::TypeSize bits =
        tti.getRegisterBitWidth(llvm::TargetTransformInfo::RGK_FixedWidthVector);
    if (bits.isScalable() || bits.getFixedValue() < 32) return 0;
    return (unsigned) (bits.getFixedValue() / 32);
}

// True iff `f` calls the named runtime wave stub.
bool callsRuntimeFn(llvm::Function& f, llvm::StringRef name) {
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = call->getCalledFunction())
                    if (cf->getName() == name) return true;
    return false;
}

// An empty `internal alwaysinline memory(none)` shell for the caller to fill.
llvm::Function* makeVariantShell(llvm::Module& m, const std::string& name,
                                 llvm::FunctionType* fnTy) {
    if (auto* existing = m.getFunction(name)) return existing;
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, &m);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->setDoesNotThrow();
    fn->setWillReturn();
    fn->setMemoryEffects(llvm::MemoryEffects::none());
    return fn;
}

// One wave op's width-W SIMD variants: an unmasked `_vW` and a masked `_Mv16`.
void attachWaveVariants(llvm::Module& m, llvm::StringRef scalarName,
                        unsigned W) {
    llvm::Function* scalar = m.getFunction(scalarName);
    if (!scalar) return;
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
    auto* maskTy = llvm::FixedVectorType::get(i1, W);
    const std::string sw = std::to_string(W);

    // The VFABI token string: `_ZGV_LLVM_N<W><tokens>_<scalar>(<unmasked>)`.
    std::string tokens;
    llvm::Function* unmasked = nullptr;
    llvm::Function* masked = nullptr;

    if (scalarName == "__cajeta_xpu_wave_reduce_sum_u32") {
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* s = b.CreateAddReduce(unmasked->getArg(0));
            b.CreateRet(b.CreateVectorSplat(W, s));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* sel = b.CreateSelect(masked->getArg(1), masked->getArg(0),
                                              llvm::Constant::getNullValue(vTy));
            llvm::Value* s = b.CreateAddReduce(sel);
            b.CreateRet(b.CreateVectorSplat(W, s));
        }
    } else if (scalarName == "__cajeta_xpu_wave_ballot_sync") {
        tokens = "v";
        auto* pTy = llvm::FixedVectorType::get(i1, W);     // per-lane predicate
        auto* rTy = llvm::FixedVectorType::get(i64, W);    // broadcast mask
        auto* iW = llvm::Type::getIntNTy(ctx, W);
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(rTy, {pTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* bits = b.CreateBitCast(unmasked->getArg(0), iW);
            llvm::Value* z = b.CreateZExt(bits, i64);
            b.CreateRet(b.CreateVectorSplat(W, z));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(rTy, {pTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* act = b.CreateAnd(masked->getArg(0), masked->getArg(1));
            llvm::Value* bits = b.CreateBitCast(act, iW);
            llvm::Value* z = b.CreateZExt(bits, i64);
            b.CreateRet(b.CreateVectorSplat(W, z));
        }
    } else if (scalarName == "__cajeta_xpu_wave_shuffle_sync_u32") {
        tokens = "vv";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        auto build = [&](llvm::Function* fn) {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
            llvm::Value* val = fn->getArg(0);
            llvm::Value* src = fn->getArg(1);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            for (unsigned i = 0; i < W; ++i) {
                llvm::Value* lane =
                    b.CreateExtractElement(src, b.getInt32(i));   // i32 src lane
                llvm::Value* picked = b.CreateExtractElement(val, lane);
                res = b.CreateInsertElement(res, picked, b.getInt32(i));
            }
            b.CreateRet(res);
        };
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy, vTy}, false));
        build(unmasked);
        masked = makeVariantShell(
            m, scalarName.str() + "_Mv" + sw,
            llvm::FunctionType::get(vTy, {vTy, vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* val = masked->getArg(0);
            llvm::Value* src = masked->getArg(1);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            for (unsigned i = 0; i < W; ++i) {
                llvm::Value* lane = b.CreateExtractElement(src, b.getInt32(i));
                llvm::Value* picked = b.CreateExtractElement(val, lane);
                res = b.CreateInsertElement(res, picked, b.getInt32(i));
            }
            b.CreateRet(res);
        }
    } else if (scalarName == "__cajeta_xpu_wave_reduce_max_u32" ||
               scalarName == "__cajeta_xpu_wave_reduce_min_u32" ||
               scalarName == "__cajeta_xpu_wave_reduce_and_u32" ||
               scalarName == "__cajeta_xpu_wave_reduce_or_u32" ||
               scalarName == "__cajeta_xpu_wave_reduce_xor_u32") {
        // Beyond sum; the masked form folds inactive lanes to the identity.
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        auto reduceOf = [&](llvm::IRBuilder<>& b, llvm::Value* x) -> llvm::Value* {
            if (scalarName == "__cajeta_xpu_wave_reduce_max_u32")
                return b.CreateIntMaxReduce(x, /*IsSigned=*/false);
            if (scalarName == "__cajeta_xpu_wave_reduce_min_u32")
                return b.CreateIntMinReduce(x, /*IsSigned=*/false);
            if (scalarName == "__cajeta_xpu_wave_reduce_and_u32")
                return b.CreateAndReduce(x);
            if (scalarName == "__cajeta_xpu_wave_reduce_or_u32")
                return b.CreateOrReduce(x);
            return b.CreateXorReduce(x);
        };
        uint32_t ident =  // AND identity = all-ones, MIN identity = UINT_MAX
            (scalarName == "__cajeta_xpu_wave_reduce_and_u32" ||
             scalarName == "__cajeta_xpu_wave_reduce_min_u32") ? 0xFFFFFFFFu : 0u;
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, unmasked->getArg(0))));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* idv = b.CreateVectorSplat(
                W, llvm::ConstantInt::get(i32, ident));
            llvm::Value* sel = b.CreateSelect(masked->getArg(1),
                                              masked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
    } else if (scalarName == "__cajeta_xpu_wave_reduce_sum_f32" ||
               scalarName == "__cajeta_xpu_wave_reduce_max_f32") {
        tokens = "v";
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        auto* vTy = llvm::FixedVectorType::get(f32, W);
        const bool isSum = scalarName == "__cajeta_xpu_wave_reduce_sum_f32";
        auto reduceOf = [&](llvm::IRBuilder<>& b, llvm::Value* x) -> llvm::Value* {
            if (isSum)
                return b.CreateFAddReduce(
                    llvm::ConstantFP::get(f32, 0.0), x);
            return b.CreateFPMaxReduce(x);
        };
        llvm::Constant* ident = isSum
            ? llvm::ConstantFP::get(f32, 0.0)
            : llvm::ConstantFP::get(f32, -3.402823466e38);
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, unmasked->getArg(0))));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* idv = b.CreateVectorSplat(W, ident);
            llvm::Value* sel = b.CreateSelect(masked->getArg(1),
                                              masked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
    } else if (scalarName.ends_with("_f32_m") &&
               scalarName.starts_with("__cajeta_xpu_wave_reduce_")) {
        llvm::StringRef base = scalarName.drop_back(2);   // strip "_m"
        tokens = "vv";
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        auto* vTy = llvm::FixedVectorType::get(f32, W);
        const bool isSum = base == "__cajeta_xpu_wave_reduce_sum_f32";
        auto reduceOf = [&](llvm::IRBuilder<>& b, llvm::Value* x) -> llvm::Value* {
            if (isSum)
                return b.CreateFAddReduce(llvm::ConstantFP::get(f32, 0.0), x);
            return b.CreateFPMaxReduce(x);
        };
        llvm::Constant* ident = isSum
            ? llvm::ConstantFP::get(f32, 0.0)
            : llvm::ConstantFP::get(f32, -3.402823466e38);
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* idv = b.CreateVectorSplat(W, ident);
            llvm::Value* sel = b.CreateSelect(unmasked->getArg(1),
                                              unmasked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy, maskTy},
                                                          false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* act = b.CreateAnd(masked->getArg(1), masked->getArg(2));
            llvm::Value* idv = b.CreateVectorSplat(W, ident);
            llvm::Value* sel = b.CreateSelect(act, masked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
    } else if (scalarName.ends_with("_u32_m") &&
               scalarName.starts_with("__cajeta_xpu_wave_reduce_")) {
        // Mask-as-data reduce (value, active): the guard rides as a DATA argument,
        // so the call sits in unconditional code and the plain `N` variant IS the
        // guarded op. `M` composes an enclosing residual predicate by AND.
        llvm::StringRef base = scalarName.drop_back(2);   // strip "_m"
        tokens = "vv";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        auto reduceOf = [&](llvm::IRBuilder<>& b, llvm::Value* x) -> llvm::Value* {
            if (base == "__cajeta_xpu_wave_reduce_sum_u32") return b.CreateAddReduce(x);
            if (base == "__cajeta_xpu_wave_reduce_max_u32")
                return b.CreateIntMaxReduce(x, /*IsSigned=*/false);
            if (base == "__cajeta_xpu_wave_reduce_min_u32")
                return b.CreateIntMinReduce(x, /*IsSigned=*/false);
            if (base == "__cajeta_xpu_wave_reduce_and_u32") return b.CreateAndReduce(x);
            if (base == "__cajeta_xpu_wave_reduce_or_u32")  return b.CreateOrReduce(x);
            return b.CreateXorReduce(x);
        };
        const uint32_t ident =
            (base == "__cajeta_xpu_wave_reduce_and_u32" ||
             base == "__cajeta_xpu_wave_reduce_min_u32") ? 0xFFFFFFFFu : 0u;
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* idv = b.CreateVectorSplat(
                W, llvm::ConstantInt::get(i32, ident));
            llvm::Value* sel = b.CreateSelect(unmasked->getArg(1),
                                              unmasked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* idv = b.CreateVectorSplat(
                W, llvm::ConstantInt::get(i32, ident));
            llvm::Value* act = b.CreateAnd(masked->getArg(1), masked->getArg(2));
            llvm::Value* sel = b.CreateSelect(act, masked->getArg(0), idv);
            b.CreateRet(b.CreateVectorSplat(W, reduceOf(b, sel)));
        }
    } else if (scalarName == "__cajeta_xpu_wave_prefix_sum_u32" ||
               scalarName == "__cajeta_xpu_wave_prefix_product_u32") {
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        bool sum = scalarName == "__cajeta_xpu_wave_prefix_sum_u32";
        uint32_t ident = sum ? 0u : 1u;
        auto op = [&](llvm::IRBuilder<>& b, llvm::Value* a, llvm::Value* x) {
            return sum ? b.CreateAdd(a, x) : b.CreateMul(a, x);
        };
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* x = unmasked->getArg(0);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            llvm::Value* acc = llvm::ConstantInt::get(i32, ident);
            for (unsigned i = 0; i < W; ++i) {
                res = b.CreateInsertElement(res, acc, b.getInt32(i));
                acc = op(b, acc, b.CreateExtractElement(x, b.getInt32(i)));
            }
            b.CreateRet(res);
        }
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* idv = b.CreateVectorSplat(
                W, llvm::ConstantInt::get(i32, ident));
            llvm::Value* x = b.CreateSelect(masked->getArg(1),
                                            masked->getArg(0), idv);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            llvm::Value* acc = llvm::ConstantInt::get(i32, ident);
            for (unsigned i = 0; i < W; ++i) {
                res = b.CreateInsertElement(res, acc, b.getInt32(i));
                acc = op(b, acc, b.CreateExtractElement(x, b.getInt32(i)));
            }
            b.CreateRet(res);
        }
    } else {
        return;
    }

    const std::string base = "_ZGV_LLVM_N" + sw + tokens + "_" + scalarName.str();
    const std::string mbase = "_ZGV_LLVM_M" + sw + tokens + "_" + scalarName.str();
    const std::string attr =
        base + "(" + unmasked->getName().str() + ")," +
        mbase + "(" + masked->getName().str() + ")";
    scalar->addFnAttr("vector-function-abi-variant", attr);
}

// `width()` takes no argument, so it cannot carry a VFABI variant. The width IS
// W in a vectorized wave kernel, so each call becomes that constant.
unsigned rewriteWaveWidth(llvm::Function& f, unsigned W) {
    llvm::SmallVector<llvm::CallInst*, 4> calls;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    if (cf->getName() == "__cajeta_xpu_wave_width")
                        calls.push_back(c);
    llvm::Value* wConst =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.getContext()), W);
    for (auto* c : calls) {
        c->replaceAllUsesWith(wConst);
        c->eraseFromParent();
    }
    return calls.size();
}

// Force a work-item loop's back-edge to width W via self-referential !llvm.loop.
void forceLoopVectorWidth(llvm::UncondBrInst* latch, unsigned W) {
    llvm::LLVMContext& ctx = latch->getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    auto md = [&](const char* key, llvm::Constant* val) {
        return llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, key),
                                       llvm::ConstantAsMetadata::get(val)});
    };
    llvm::SmallVector<llvm::Metadata*, 4> ops;
    ops.push_back(nullptr);  // self-reference, patched below
    ops.push_back(md("llvm.loop.vectorize.width", llvm::ConstantInt::get(i32, W)));
    ops.push_back(md("llvm.loop.vectorize.enable", llvm::ConstantInt::getTrue(ctx)));
    // Names this loop as a WORK-ITEM loop. LoopVectorize keeps a hint it does
    // not own on both the vector loop and the scalar remainder, so after the
    // pass the left-scalar gate can find the work-item loop a wave call sits
    // in and ask whether THAT loop was widened -- an inner per-lane loop the
    // vectorizer happened to take instead also reads as `isvectorized`, and
    // would otherwise pass a shuffle that still runs as the identity.
    ops.push_back(llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "cajeta.xpu.wi")}));
    llvm::MDNode* loopId = llvm::MDNode::getDistinct(ctx, ops);
    loopId->replaceOperandWith(0, loopId);
    latch->setMetadata("llvm.loop", loopId);
}

// A work-item loop is PARALLEL by the kernel model: its iterations are the
// block's work-items, and within one barrier region they are independent (a
// cross-work-item race there is undefined on every backend). LoopVectorize
// does not know that: it runs LoopAccessInfo, which must prove or runtime-
// check every buffer store against every buffer load, and a distributed tile
// stores through addresses like `g + 2*i` with g = lane/16, which is not
// affine in the work-item induction variable -- "cannot identify array
// bounds" -- so the loop stays scalar and the wave op inside runs as the
// width-1 identity stub (measured 2026-09-23 on every distributed probe and
// on q6kWmmaDeqMw4Kernel). Tagging every memory access in the loop with one
// access group and listing that group in llvm.loop.parallel_accesses is the
// `omp simd` contract: independence across THIS loop's iterations only. An
// access inside a nested loop is tagged too -- it is still independent across
// work-items -- but the nested loop's own ID does not list the group, so its
// loop-carried dependences (a k reduction, a scan) are untouched.
static bool loopHasHint(llvm::Loop* L, llvm::StringRef hint);
static void markWorkItemLoopsParallel(llvm::Function& f) {
    llvm::LLVMContext& ctx = f.getContext();
    llvm::DominatorTree dt(f);
    llvm::LoopInfo li(dt);
    llvm::SmallVector<llvm::Loop*, 8> loops;
    for (llvm::Loop* top : li)
        for (llvm::Loop* L : llvm::depth_first(top))
            if (loopHasHint(L, "cajeta.xpu.wi")) loops.push_back(L);
    for (llvm::Loop* L : loops) {
        llvm::MDNode* group = llvm::MDNode::getDistinct(ctx, {});
        for (llvm::BasicBlock* bb : L->blocks())
            for (llvm::Instruction& in : *bb) {
                if (!in.mayReadOrWriteMemory()) continue;
                if (auto* c = llvm::dyn_cast<llvm::CallBase>(&in))
                    if (!c->getCalledFunction() || !c->getCalledFunction()->isIntrinsic())
                        continue;               // an opaque call keeps its own effects
                in.setMetadata(llvm::LLVMContext::MD_access_group, group);
            }
        llvm::MDNode* id = L->getLoopID();
        llvm::SmallVector<llvm::Metadata*, 6> ops;
        ops.push_back(nullptr);
        if (id)
            for (unsigned i = 1; i < id->getNumOperands(); ++i)
                ops.push_back(id->getOperand(i));
        ops.push_back(llvm::MDNode::get(
            ctx, {llvm::MDString::get(ctx, "llvm.loop.parallel_accesses"), group}));
        llvm::MDNode* newId = llvm::MDNode::getDistinct(ctx, ops);
        newId->replaceOperandWith(0, newId);
        L->setLoopID(newId);
    }
}

// The wave stubs that carry a VFABI variant; width() and lane_id are elsewhere.
static const char* const kWaveOps[] = {
    "__cajeta_xpu_wave_reduce_sum_u32",
    "__cajeta_xpu_wave_reduce_max_u32",
    "__cajeta_xpu_wave_reduce_sum_f32",
    "__cajeta_xpu_wave_reduce_max_f32",
    "__cajeta_xpu_wave_reduce_min_u32",
    "__cajeta_xpu_wave_reduce_and_u32",
    "__cajeta_xpu_wave_reduce_or_u32",
    "__cajeta_xpu_wave_reduce_xor_u32",
    "__cajeta_xpu_wave_prefix_sum_u32",
    "__cajeta_xpu_wave_prefix_product_u32",
    "__cajeta_xpu_wave_ballot_sync",
    "__cajeta_xpu_wave_shuffle_sync_u32",
};

// ── Mask-as-data rewrite (divergent wave calls) ────────────────────────────
// A guarded wave reduce that LoopVectorize SCALARIZES is a width-1 identity, silently
// wrong, so the guard becomes a DATA argument on an unconditional call in the merge block.


// CAJETA_XPU_DEBUG_WAVE: surface LoopVectorize's remarks; no in-process flag.
namespace {
struct WaveRemarkHandler final : public llvm::DiagnosticHandler {
    bool handleDiagnostics(const llvm::DiagnosticInfo& di) override {
        if (auto* opt = llvm::dyn_cast<llvm::DiagnosticInfoOptimizationBase>(
                const_cast<llvm::DiagnosticInfo*>(&di))) {
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << opt->getPassName() << ": " << opt->getMsg();
            fprintf(stderr, "[wave-remark] %s\n", os.str().c_str());
            return true;
        }
        return false;
    }
    bool isAnalysisRemarkEnabled(llvm::StringRef) const override { return true; }
    bool isMissedOptRemarkEnabled(llvm::StringRef) const override { return true; }
    bool isPassedOptRemarkEnabled(llvm::StringRef) const override { return true; }
    bool isAnyRemarkEnabled() const override { return true; }
};
} // namespace

static bool isMaskAsDataReduce(llvm::StringRef name, bool* isMasked) {
    if (!name.starts_with("__cajeta_xpu_wave_reduce_")) return false;
    if (name.ends_with("_u32") || name.ends_with("_f32")) {
        *isMasked = false;
        return true;
    }
    if (name.ends_with("_u32_m") || name.ends_with("_f32_m")) {
        *isMasked = true;
        return true;
    }
    return false;
}

static bool rewriteOneGuardedWaveCall(llvm::Function& f, llvm::Module& m,
                                      unsigned waveW) {
    llvm::LLVMContext& ctx = f.getContext();
    llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);

    for (llvm::BasicBlock& bb : f) {
        llvm::CallInst* call = nullptr;
        bool wasMasked = false;
        for (llvm::Instruction& in : bb)
            if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = ci->getCalledFunction())
                    if (isMaskAsDataReduce(cf->getName(), &wasMasked)) {
                        call = ci;
                        break;
                    }
        if (!call) continue;

        // Triangle guard: unique condbr pred P, unique succ J = P's other edge.
        llvm::BasicBlock* pred = bb.getSinglePredecessor();
        if (!pred) continue;
        auto* pbr = llvm::dyn_cast<llvm::BranchInst>(pred->getTerminator());
        if (!pbr || !pbr->isConditional()) continue;
        llvm::BasicBlock* succ = bb.getSingleSuccessor();
        if (!succ) continue;
        const bool onTrue = pbr->getSuccessor(0) == &bb;
        llvm::BasicBlock* other =
            onTrue ? pbr->getSuccessor(1) : pbr->getSuccessor(0);
        if (other != succ) {
            if (getenv("CAJETA_XPU_DEBUG_WAVE"))
                fprintf(stderr, "[wave-msd] %s: guarded wave call not in "
                        "triangle shape — left predicated\n",
                        f.getName().str().c_str());
            continue;
        }
        llvm::Value* cond = pbr->getCondition();

        // Resolve the _m stub BEFORE any surgery: a type mismatch must bail here.
        llvm::Function* calleeF = call->getCalledFunction();
        const std::string mName = wasMasked
            ? calleeF->getName().str()
            : calleeF->getName().str() + "_m";
        // The scalar carrier follows the family: i32, or float for the f32 twins.
        llvm::Type* sTy = call->getType();
        auto* mTy = llvm::FunctionType::get(sTy, {sTy, i1}, false);
        llvm::Function* mf = m.getFunction(mName);
        if (mf && mf->getFunctionType() != mTy) {
            if (getenv("CAJETA_XPU_DEBUG_WAVE"))
                fprintf(stderr, "[wave-msd] %s: unexpected signature on %s — "
                        "left predicated\n",
                        f.getName().str().c_str(), mName.c_str());
            continue;
        }

        // Split: bb keeps the pre-call code (B1); the call and the rest move to B2.
        llvm::BasicBlock* b1 = &bb;
        llvm::BasicBlock* b2 =
            b1->splitBasicBlock(call->getIterator(), b1->getName() + ".wave.tail");

        // Merge block M — every path now runs it.
        llvm::BasicBlock* mblk = llvm::BasicBlock::Create(
            ctx, b1->getName() + ".wave.msd", &f, b2);
        b1->getTerminator()->setSuccessor(0, mblk);          // B1 -> M
        pbr->setSuccessor(onTrue ? 1 : 0, mblk);             // P bypass -> M
        succ->replacePhiUsesWith(pred, mblk);                // J phis: P -> M

        // Lift B1 values used outside B1 through an M phi: B1 no longer dominates.
        for (llvm::Instruction& in : *b1) {
            if (in.isTerminator()) continue;
            llvm::SmallVector<llvm::Use*, 8> outside;
            for (llvm::Use& u : in.uses()) {
                auto* userIn = llvm::cast<llvm::Instruction>(u.getUser());
                if (userIn->getParent() != b1) outside.push_back(&u);
            }
            if (outside.empty()) continue;
            auto* phi = llvm::PHINode::Create(in.getType(), 2,
                                              in.getName() + ".msd", mblk);
            phi->addIncoming(&in, b1);
            phi->addIncoming(llvm::PoisonValue::get(in.getType()), pred);
            for (llvm::Use* u : outside) u->set(phi);
        }

        // The lane-active mask: true down the guarded edge, false down the bypass.
        auto* mask = llvm::PHINode::Create(i1, 2, "wave.mask", mblk);
        llvm::Value* innerActive = llvm::ConstantInt::getTrue(ctx);
        if (wasMasked) {
            llvm::Value* om = call->getArgOperand(1);
            // The operand may already be an M phi; take its value as seen at B1.
            if (auto* omPhi = llvm::dyn_cast<llvm::PHINode>(om))
                if (omPhi->getParent() == mblk)
                    om = omPhi->getIncomingValueForBlock(b1);
            innerActive = om;
        }
        mask->addIncoming(innerActive, b1);
        mask->addIncoming(llvm::ConstantInt::getFalse(ctx), pred);

        // The unconditional masked call. Its type was verified before surgery: a
        // bitcast ConstantExpr callee fails LoopVectorize's legality, which kills
        // vectorization of the whole wrapper.
        llvm::FunctionCallee mCallee =
            mf ? llvm::FunctionCallee(mf) : m.getOrInsertFunction(mName, mTy);
        if (auto* mfn = llvm::dyn_cast<llvm::Function>(mCallee.getCallee())) {
            mfn->setDoesNotThrow();
            mfn->setWillReturn();
            mfn->setMemoryEffects(llvm::MemoryEffects::none());
            attachWaveVariants(m, mName, waveW);
        }
        llvm::IRBuilder<> b(mblk);
        auto* newCall = b.CreateCall(mCallee,
                                     {call->getArgOperand(0), mask},
                                     call->getName() + ".msd");
        newCall->setDoesNotThrow();
        // Match the C definition's `_Bool` ABI: a zero-extended mask register.
        newCall->addParamAttr(1, llvm::Attribute::ZExt);

        // Re-guard the remainder of the original block.
        llvm::Value* enter = cond;
        if (!onTrue) enter = b.CreateNot(cond, "wave.msd.not");
        b.CreateCondBr(enter, b2, succ);

        call->replaceAllUsesWith(newCall);
        call->eraseFromParent();
        return true;
    }
    return false;
}

static void waveMaskAsData(llvm::Function& f, llvm::Module& m, unsigned waveW) {
    // Fixpoint: each rewrite may expose the new call to an enclosing guard.
    for (unsigned i = 0; i < 64; ++i)
        if (!rewriteOneGuardedWaveCall(f, m, waveW)) break;
}

// Attach SIMD variants for every wave op `f` uses; reports a wave kernel.

// CAJETA_XPU_DEBUG_WAVE=1: dump the wrapper IR after vectorization and folding.
static void maybeDumpWaveWrapper(const llvm::Function& f, unsigned waveW) {
    if (!getenv("CAJETA_XPU_DEBUG_WAVE")) return;
    std::string out;
    llvm::raw_string_ostream os(out);
    f.print(os);
    // Also show every wave-stub declaration and its VFABI attribute.
    for (const llvm::Function& g : *f.getParent())
        if (g.getName().contains("__cajeta_xpu_wave_")) {
            auto attr = g.getFnAttribute("vector-function-abi-variant");
            fprintf(stderr, "[wave-decl] %s -> %s\n", g.getName().str().c_str(),
                    attr.isValid() ? attr.getValueAsString().str().c_str()
                                   : "(no VFABI attr)");
        }
    fprintf(stderr, "[wave-dump] %s (W=%u)\n%s\n[wave-dump-end] %s\n",
            f.getName().str().c_str(), waveW, os.str().c_str(),
            f.getName().str().c_str());
}

bool setupWaveVariants(llvm::Function& f, llvm::Module& m, unsigned waveW) {
    bool waveKernel = false;
    for (const char* op : kWaveOps)
        if (callsRuntimeFn(f, op)) {
            // In a JIT build the runtime bitcode is linked in BEFORE this
            // runs, so the width-1 stub arrives as a DEFINITION whose value
            // parameter clang marked `returned` (`return value;`). LLVM then
            // folds every call to its argument -- the identity shuffle --
            // and deletes the dead call before LoopVectorize can widen it or
            // the gate can see it (measured 2026-09-23: every JIT probe was
            // silently identity while the same kernel was right in an exe
            // build, where the stub is a declaration until LTO). Strip the
            // attribute from the stub and its call sites so both builds see
            // an opaque scalar call.
            if (llvm::Function* stub = m.getFunction(op)) {
                for (unsigned i = 0; i < stub->arg_size(); ++i)
                    stub->removeParamAttr(i, llvm::Attribute::Returned);
                for (llvm::User* u : stub->users())
                    if (auto* c = llvm::dyn_cast<llvm::CallBase>(u))
                        for (unsigned i = 0; i < c->arg_size(); ++i)
                            c->removeParamAttr(i, llvm::Attribute::Returned);
            }
            attachWaveVariants(m, op, waveW);
            waveKernel = true;
        }
    if (callsRuntimeFn(f, "__cajeta_xpu_wave_width")) waveKernel = true;
    return waveKernel;
}

// LoopVectorize refuses any loop holding a callsite it cannot widen: a call to
// a cajeta method the kernel body still names (QuantKernel.scaleAtDev,
// GgufFile.halfBitsToF32) has no vector variant and is not an intrinsic, so the
// work-item loop stays scalar and every wave op in it runs its width-1 stub.
// That is how the whole wave mat-vec family came to register on the CPU with
// no vector body at all (measured 2026-09-23: f16F32WaveMatVecKernel, eight
// halfBitsToF32 calls, one scalar wave_reduce_sum_f32). Inline every device
// callee with a body into the wrapper, to a fixpoint, before vectorizing.
static unsigned inlineDeviceCallees(llvm::Function& wrapper) {
    unsigned inlined = 0;
    for (unsigned round = 0; round < 32; ++round) {
        llvm::SmallVector<llvm::CallInst*, 16> calls;
        for (auto& bb : wrapper)
            for (auto& in : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (auto* cf = c->getCalledFunction())
                        if (!cf->isDeclaration() && !cf->isIntrinsic()
                            && cf->getName().starts_with("__cajeta_xpu_dev."))
                            calls.push_back(c);
        if (calls.empty()) break;
        for (llvm::CallInst* c : calls) {
            llvm::InlineFunctionInfo ifi;
            if (llvm::InlineFunction(*c, ifi).isSuccess()) ++inlined;
        }
    }
    return inlined;
}

// A wave op LoopVectorize left as its scalar stub runs with width-1 semantics:
// a shuffle is the identity, a reduce is its own input. That is silently wrong
// for a wave of W and is how q6kWmmaDeqMw4Kernel's mma came to read each
// lane's own A element for every k on the CPU (2026-09-23). Every work-item
// loop is forced to width W; the ones LoopVectorize widened carry
// llvm.loop.isvectorized, their scalar remainder loops too. A scalar wave call
// under no such loop is a region that did not vectorize, and the kernel must
// be refused rather than registered.
static bool loopHasHint(llvm::Loop* L, llvm::StringRef hint) {
    llvm::MDNode* id = L->getLoopID();
    if (!id) return false;
    for (unsigned i = 1; i < id->getNumOperands(); ++i)
        if (auto* md = llvm::dyn_cast<llvm::MDNode>(id->getOperand(i)))
            if (md->getNumOperands() > 0)
                if (auto* str = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                    if (str->getString() == hint) return true;
    return false;
}
// The WORK-ITEM loop this call sits in (the nearest enclosing loop tagged
// cajeta.xpu.wi) must itself carry llvm.loop.isvectorized. An inner loop the
// vectorizer widened instead does not count, and a wave call under no
// work-item loop at all is scaffold code that never had a wave.
static bool loopChainVectorized(llvm::Loop* L) {
    for (; L; L = L->getParentLoop())
        if (loopHasHint(L, "cajeta.xpu.wi"))
            return loopHasHint(L, "llvm.loop.isvectorized");
    return false;
}
static bool waveOpLeftScalar(llvm::Function& f, std::string* which) {
    llvm::DominatorTree dt(f);
    llvm::LoopInfo li(dt);
    const bool dbg = getenv("CAJETA_XPU_DEBUG_WAVE") != nullptr;
    unsigned scalarCalls = 0;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    for (const char* op : kWaveOps)
                        if (cf->getName() == op) {
                            ++scalarCalls;
                            llvm::Loop* L = li.getLoopFor(&bb);
                            bool ok = loopChainVectorized(L);
                            if (dbg && scalarCalls <= 4) {
                                std::string chain;
                                for (llvm::Loop* P = L; P; P = P->getParentLoop())
                                    chain += (P->getHeader()->hasName()
                                                  ? P->getHeader()->getName().str()
                                                  : std::string("<hdr>"))
                                           + (loopHasHint(P, "cajeta.xpu.wi") ? "[wi" : "[")
                                           + (loopHasHint(P, "llvm.loop.isvectorized")
                                                  ? ",vec] " : "] ");
                                fprintf(stderr, "[wave-gate] %s: scalar %s in %s -> loops %s=> %s\n",
                                        f.getName().str().c_str(), op,
                                        bb.hasName() ? bb.getName().str().c_str() : "<bb>",
                                        chain.c_str(), ok ? "widened wi loop" : "LEFT SCALAR");
                            }
                            if (!ok) { if (which) *which = op; return true; }
                        }
    if (dbg) fprintf(stderr, "[wave-gate] %s: %u scalar wave calls, all under widened work-item loops\n",
                     f.getName().str().c_str(), scalarCalls);
    return false;
}

// Fold the substituted variant calls into the loop; no inliner runs at -O0.
void foldWaveVariants(llvm::Function& f) {
    llvm::SmallVector<llvm::CallInst*, 16> vcalls;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    if (cf->hasFnAttribute(llvm::Attribute::AlwaysInline)
                        && cf->getName().contains("__cajeta_xpu_wave_"))
                        vcalls.push_back(c);
    for (auto* c : vcalls) {
        llvm::InlineFunctionInfo vifi;
        llvm::InlineFunction(*c, vifi);
    }
}

} // namespace

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& /*arch*/,
                               std::vector<KernelManifest>* manifests) {
        if (kernels.empty()) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_cpu_kernel(i8* name, i8* fn)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
        llvm::FunctionCallee regFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_cpu_kernel", regTy);

        // Host TargetMachine, for LoopVectorize's TTI. Null is tolerated.
        std::unique_ptr<llvm::TargetMachine> hostTm = createCpuTargetMachine();

        // The wrapper's 9 coordinate params; nctaid rides through for gridSize().
        const unsigned kNumBlockCoordParams = 9;

        // A kernel's LLVM symbols must be unique per PROGRAM while the runtime
        // registry is keyed by the SIMPLE name, so symbols are qualified by the
        // declaring class and the registry collision is diagnosed here.
        auto symSuffix = [](const cajeta::MethodPtr& m) {
            std::string q = m->getParent() ? m->getParent()->toCanonical()
                                           : std::string();
            std::string full = q.empty() ? m->getName() : q + "." + m->getName();
            for (char& c : full)
                if (!std::isalnum((unsigned char) c) && c != '.' && c != '_')
                    c = '_';
            return full;
        };
        // Only a collision WITHIN one unit; cross-file shadowing stays open.
        std::map<std::string, std::string> simpleNameOwner;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            std::string owner = method->getParent()
                ? method->getParent()->toCanonical() : std::string("<none>");
            auto ins = simpleNameOwner.emplace(method->getName(), owner);
            if (!ins.second && ins.first->second != owner) {
                throw cajeta::Exception(
                    "two @Kernel methods share the simple name '"
                    + method->getName() + "' (" + ins.first->second + " and "
                    + owner + "). Kernel launches resolve by simple name, so "
                    "one would silently shadow the other; rename one.",
                    "CAJETA_ERROR_XPU_KERNEL_NAME_COLLISION");
            }
        }

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();
            const std::string sym = "__cajeta_xpu_cpu." + symSuffix(method);

            // A fresh module sharing the host context; a throw leaves host clean.
            auto mod = std::make_unique<llvm::Module>("xpu.cpu." + entryName, ctx);
            mod->setDataLayout(hostModule.getDataLayout());
            mod->setTargetTriple(hostModule.getTargetTriple());
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, *mod);
            } catch (cajeta::Exception& e) {
                // A contradicted @Access is the author's error: a compile error.
                if (e.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_CONTRADICTED"
                        || e.getErrorId() == "CAJETA_ERROR_XPU_ACCESS_UNKNOWN") throw;
                // XPU-N01: this kernel gets NO CPU code, and a @Kernel has no host
                // fallback — the launch finds nothing and every output reads ZERO.
                // Say so at build time, as the other backends already do.
                fprintf(stderr,
                        "cajeta: note: [xpu-kernel-skipped] %s: no cpu device "
                        "code — %s (%s)\n",
                        entryName.c_str(), e.getMessage().c_str(),
                        e.getErrorId().c_str());
                continue;
            }
            if (!kfn) continue;
            kfn->setName(sym);
            kfn->setLinkage(llvm::GlobalValue::ExternalLinkage);

            // Identity only: the CPU has no VGPR or LDS footprint to report.
            KernelManifest manifest;
            {
                std::string ir;
                llvm::raw_string_ostream os(ir);
                mod->print(os, nullptr);
                os.flush();
                manifest.kernel = qualifiedKernelName(method);
                manifest.target = "cpu/" + (hostTm ? hostTm->getTargetCPU().str()
                                                   : std::string("unknown"));
                manifest.codeHash = sha256Hex(
                    reinterpret_cast<const uint8_t*>(ir.data()), ir.size());
                manifest.compilerVersion = compilerVersionString();
                manifest.xpuAbiVersion = CAJETA_XPU_ABI_VERSION;
                // Access modes off the lowered body, before fission rewrites it.
                applyAccess(manifest, classifyKernelAccess(*kfn, method));
                applyNativeOps(manifest, kfn);
            }

            if (llvm::Linker::linkModules(hostModule, std::move(mod))) {
                continue;  // link error (logged by the linker)
            }
            llvm::Function* linked = hostModule.getFunction(sym);
            if (!linked) continue;
            linked->addFnAttr(llvm::Attribute::AlwaysInline);   // inline into loop

            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            llvm::FunctionType* kfnTy = linked->getFunctionType();
            const unsigned total = kfnTy->getNumParams();
            const unsigned nReal = total - kNumCoordParams;     // buffers + scalars

            // --- Per-block wrapper (POCL model) -----------------------------
            // Loops tid.x over [0, ntid.x) calling the per-work-item kernel, which is
            // then inlined and handed to LoopVectorize; the launch ABI is 1-D.
            std::vector<llvm::Type*> wtys;
            wtys.reserve(nReal + kNumBlockCoordParams + 1);
            for (unsigned i = 0; i < nReal; ++i)
                wtys.push_back(kfnTy->getParamType(i));
            for (unsigned i = 0; i < kNumBlockCoordParams; ++i)
                wtys.push_back(i32);
            wtys.push_back(i32);            // dynamic shared-memory byte count
            llvm::Function* wrapper = llvm::Function::Create(
                llvm::FunctionType::get(voidTy, wtys, false),
                llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_block." + symSuffix(method), hostModule);
            // The trailing wrapper param is the dynamic shared byte count.
            llvm::Value* dynSharedBytes = wrapper->getArg(nReal + kNumBlockCoordParams);

            llvm::Value* ctaidX = wrapper->getArg(nReal + 0);
            llvm::Value* ctaidY = wrapper->getArg(nReal + 1);
            llvm::Value* ctaidZ = wrapper->getArg(nReal + 2);
            llvm::Value* ntidX  = wrapper->getArg(nReal + 3);
            llvm::Value* ntidY  = wrapper->getArg(nReal + 4);
            llvm::Value* ntidZ  = wrapper->getArg(nReal + 5);
            llvm::Value* nctaidX = wrapper->getArg(nReal + 6);
            llvm::Value* nctaidY = wrapper->getArg(nReal + 7);
            llvm::Value* nctaidZ = wrapper->getArg(nReal + 8);

            // A kernel calling Barrier.workgroup() cannot be one work-item loop:
            // it is split at each barrier into regions, each looped over the block
            // as a 3-D nest. XPU-N02 discards and falls back to the host stub.
            // A wave kernel with a loop in its body takes the fission path as
            // well, barrier or not: its cross-lane ops widen only when the
            // work-item loop is innermost, and fission makes each workgroup-
            // uniform loop scaffold so that it is (CpuBarrierFission.cpp 4b).
            bool waveOpsUsed = false;
            for (const char* op : kWaveOps)
                if (callsRuntimeFn(*linked, op)) { waveOpsUsed = true; break; }
            bool bodyHasLoop = false;
            {
                llvm::DominatorTree ldt(*linked);
                llvm::LoopInfo lli(ldt);
                bodyHasLoop = !lli.empty();
            }
            if (usesBarrier(*linked) || (waveOpsUsed && bodyHasLoop)) {
                std::vector<llvm::UncondBrInst*> wiLatches;
                std::vector<llvm::Value*> ctaidV = {ctaidX, ctaidY, ctaidZ};
                std::vector<llvm::Value*> ntidV  = {ntidX,  ntidY,  ntidZ};
                std::vector<llvm::Value*> nctaidV = {nctaidX, nctaidY, nctaidZ};
                // Uniform-loop scaffolding is what lets a wave kernel's loops
                // vectorize; a shape it cannot region is retried the plain
                // way, on a fresh wrapper, and the left-scalar gate below then
                // decides whether that plain lowering may register.
                bool scaffold = waveOpsUsed;
                bool fissioned = false;
                while (!fissioned) {
                    try {
                        fissionBarrierKernel(linked, wrapper, nReal, ctaidV, ntidV,
                                             nctaidV, hostModule, &wiLatches,
                                             dynSharedBytes,
                                             /*scaffoldUniformLoops=*/scaffold);
                        fissioned = true;
                    } catch (cajeta::Exception& e) {
                        if (scaffold) {
                            fprintf(stderr,
                                    "cajeta: note: [xpu-kernel-fission] %s: uniform-"
                                    "loop scaffolding declined (%s); retrying the "
                                    "plain fission\n",
                                    entryName.c_str(), e.getMessage().c_str());
                            scaffold = false;
                            wiLatches.clear();
                            wrapper->eraseFromParent();
                            wrapper = llvm::Function::Create(
                                llvm::FunctionType::get(voidTy, wtys, false),
                                llvm::GlobalValue::ExternalLinkage,
                                "__cajeta_xpu_cpu_block." + symSuffix(method),
                                hostModule);
                            dynSharedBytes = wrapper->getArg(nReal + kNumBlockCoordParams);
                            ctaidX = wrapper->getArg(nReal + 0);
                            ctaidY = wrapper->getArg(nReal + 1);
                            ctaidZ = wrapper->getArg(nReal + 2);
                            ntidX  = wrapper->getArg(nReal + 3);
                            ntidY  = wrapper->getArg(nReal + 4);
                            ntidZ  = wrapper->getArg(nReal + 5);
                            nctaidX = wrapper->getArg(nReal + 6);
                            nctaidY = wrapper->getArg(nReal + 7);
                            nctaidZ = wrapper->getArg(nReal + 8);
                            ctaidV = {ctaidX, ctaidY, ctaidZ};
                            ntidV  = {ntidX,  ntidY,  ntidZ};
                            nctaidV = {nctaidX, nctaidY, nctaidZ};
                            continue;
                        }
                        // Say so at build time: a swallowed fission failure
                        // leaves a kernel with no CPU code, in silence.
                        fprintf(stderr,
                                "cajeta: note: [xpu-kernel-skipped] %s: no cpu device "
                                "code — barrier fission: %s (%s)\n",
                                entryName.c_str(), e.getMessage().c_str(),
                                e.getErrorId().c_str());
                        wrapper->eraseFromParent();
                        linked->eraseFromParent();    // has barrier markers; unusable
                        break;
                    }
                }
                if (!fissioned) continue;             // host-stub fallback
                // The distributed-coop wave-width marker rides the kernel; the
                // body has been cloned into the wrapper and the kernel is about
                // to be erased, so carry the marker onto the wrapper (cloning
                // does not copy the callee's fn-attrs) or cpuVectorWidthI32 would
                // lose it and force the fission regions to the host width.
                if (linked->hasFnAttribute("cajeta.xpu.coop-wavew"))
                    wrapper->addFnAttr(
                        linked->getFnAttribute("cajeta.xpu.coop-wavew"));
                linked->eraseFromParent();        // body cloned into the wrapper
                // Fission redirected only the wrapper's uses of the kernel's
                // `shared` globals (so a declined attempt could be retried);
                // with the kernel gone, a global nothing names comes out.
                for (llvm::GlobalVariable& gv :
                     llvm::make_early_inc_range(hostModule.globals()))
                    if (gv.getAddressSpace() == 3 && gv.use_empty())
                        gv.eraseFromParent();

                // Wave + barrier composition: each fission region is a clean
                // counted work-item loop, so each vectorizes at width W exactly as
                // the 5C path does, with the barriers delimiting the regions.
                inlineDeviceCallees(*wrapper);
                const unsigned waveW = cpuVectorWidthI32(hostTm.get(), *wrapper);
                bool waveKernel = false;
                if (waveW >= 2) {
                    waveKernel = setupWaveVariants(*wrapper, hostModule, waveW);
                    if (waveKernel) {
                        rewriteWaveWidth(*wrapper, waveW);
                        waveMaskAsData(*wrapper, hostModule, waveW);
                        for (llvm::UncondBrInst* latch : wiLatches)
                            forceLoopVectorWidth(latch, waveW);
                    }
                }
                // CAJETA_XPU_CPU_DUMP_PREOPT=<dir>: the module before vectorize.
                if (const char* dumpDir =
                        std::getenv("CAJETA_XPU_CPU_DUMP_PREOPT")) {
                    std::error_code ec;
                    llvm::raw_fd_ostream os(
                        std::string(dumpDir) + "/" +
                            wrapper->getName().str() + ".preopt.ll",
                        ec);
                    if (!ec) hostModule.print(os, nullptr);
                }
                if (!cpuVectorizeDisabled()) {
                    // CAJETA_XPU_DEBUG_WAVE: LoopVectorize's own remarks say
                    // WHY a region stayed scalar (a callsite with no vector
                    // variant, an inner loop, a dependence), which the dump
                    // after the fact cannot.
                    std::unique_ptr<llvm::DiagnosticHandler> saved;
                    const bool dbg = getenv("CAJETA_XPU_DEBUG_WAVE") && waveKernel;
                    if (dbg) {
                        saved = ctx.getDiagnosticHandler();
                        ctx.setDiagnosticHandler(std::make_unique<WaveRemarkHandler>());
                        fprintf(stderr, "[wave-remarks] %s\n", entryName.c_str());
                    }
                    if (waveKernel) markWorkItemLoopsParallel(*wrapper);
                    vectorizeFunction(*wrapper, hostTm.get());
                    if (dbg) ctx.setDiagnosticHandler(std::move(saved));
                }
                if (waveKernel) {
                    // The gate runs BEFORE the variants are folded: in a JIT
                    // build the scalar stubs arrive with bodies (the runtime
                    // bitcode is linked in first) and folding would inline a
                    // never-widened stub -- an identity shuffle -- and erase
                    // the very call the gate looks for (measured 2026-09-23).
                    std::string op;
                    if (!cpuVectorizeDisabled() && waveOpLeftScalar(*wrapper, &op)) {
                        fprintf(stderr,
                                "cajeta: note: [xpu-kernel-skipped] %s: no cpu device "
                                "code: %s was left scalar (a work-item loop did not "
                                "vectorize at the wave width %u), which would run the "
                                "wave op with width-1 semantics\n",
                                entryName.c_str(), op.c_str(), waveW);
                        wrapper->eraseFromParent();
                        continue;                     // host-stub fallback
                    }
                    foldWaveVariants(*wrapper);
                    maybeDumpWaveWrapper(*wrapper, waveW);
                }
            } else {
            // 3-D work-item loop nest over (tid.z, tid.y, tid.x). The innermost
            // tid.x loop is the vectorizable one, so a 1-D block runs as before
            // (z/y single-trip) and a 2-D/3-D block runs the whole nest.
            llvm::BasicBlock* wEntry =
                llvm::BasicBlock::Create(ctx, "entry", wrapper);
            llvm::BasicBlock* zHead =
                llvm::BasicBlock::Create(ctx, "wi.z.head", wrapper);
            llvm::BasicBlock* yHead =
                llvm::BasicBlock::Create(ctx, "wi.y.head", wrapper);
            llvm::BasicBlock* wHead =
                llvm::BasicBlock::Create(ctx, "wi.head", wrapper);
            llvm::BasicBlock* wBody =
                llvm::BasicBlock::Create(ctx, "wi.body", wrapper);
            llvm::BasicBlock* yLatch =
                llvm::BasicBlock::Create(ctx, "wi.y.latch", wrapper);
            llvm::BasicBlock* zLatch =
                llvm::BasicBlock::Create(ctx, "wi.z.latch", wrapper);
            llvm::BasicBlock* wExit =
                llvm::BasicBlock::Create(ctx, "wi.exit", wrapper);
            llvm::Constant* zero = llvm::ConstantInt::get(i32, 0);
            llvm::Constant* one  = llvm::ConstantInt::get(i32, 1);

            b.SetInsertPoint(wEntry);
            b.CreateBr(zHead);

            b.SetInsertPoint(zHead);
            llvm::PHINode* tz = b.CreatePHI(i32, 2, "tid.z");
            tz->addIncoming(zero, wEntry);
            b.CreateCondBr(b.CreateICmpSLT(tz, ntidZ, "wi.z.cond"), yHead, wExit);

            b.SetInsertPoint(yHead);
            llvm::PHINode* ty = b.CreatePHI(i32, 2, "tid.y");
            ty->addIncoming(zero, zHead);
            b.CreateCondBr(b.CreateICmpSLT(ty, ntidY, "wi.y.cond"), wHead, zLatch);

            b.SetInsertPoint(wHead);
            llvm::PHINode* tid = b.CreatePHI(i32, 2, "tid.x");
            tid->addIncoming(zero, yHead);
            b.CreateCondBr(b.CreateICmpSLT(tid, ntidX, "wi.cond"), wBody, yLatch);

            b.SetInsertPoint(wBody);
            std::vector<llvm::Value*> kArgs;
            kArgs.reserve(total);
            for (unsigned i = 0; i < nReal; ++i) kArgs.push_back(wrapper->getArg(i));
            kArgs.push_back(tid);                     // tid.x
            kArgs.push_back(ty);                      // tid.y
            kArgs.push_back(tz);                      // tid.z
            kArgs.push_back(ctaidX); kArgs.push_back(ctaidY); kArgs.push_back(ctaidZ);
            kArgs.push_back(ntidX);  kArgs.push_back(ntidY);  kArgs.push_back(ntidZ);
            kArgs.push_back(nctaidX); kArgs.push_back(nctaidY); kArgs.push_back(nctaidZ);
            llvm::CallInst* kcall = b.CreateCall(linked, kArgs);
            tid->addIncoming(b.CreateAdd(tid, one, "tid.next"), wBody);
            llvm::UncondBrInst* latchBr = b.CreateBr(wHead);   // inner (x) back-edge

            b.SetInsertPoint(yLatch);
            ty->addIncoming(b.CreateAdd(ty, one, "tid.y.next"), yLatch);
            b.CreateBr(yHead);

            b.SetInsertPoint(zLatch);
            tz->addIncoming(b.CreateAdd(tz, one, "tid.z.next"), zLatch);
            b.CreateBr(zHead);

            b.SetInsertPoint(wExit);
            b.CreateRetVoid();

            // --- Wave-op SIMD setup ----------------------------------------
            // VFABI variants and the forced loop width W both go in BEFORE inlining.
            const unsigned waveW = cpuVectorWidthI32(hostTm.get(), *wrapper);
            bool waveKernel = false;
            if (waveW >= 2) {
                waveKernel = setupWaveVariants(*linked, hostModule, waveW);
                if (waveKernel) forceLoopVectorWidth(latchBr, waveW);
            }

            // Inline the kernel into the loop body, then mem2reg + LoopVectorize.
            llvm::InlineFunctionInfo ifi;
            llvm::InlineFunction(*kcall, ifi);
            inlineDeviceCallees(*wrapper);
            // A wave op that arrived from an inlined helper needs its variants
            // and the forced width as much as one the kernel named directly.
            if (waveW >= 2) {
                bool more = setupWaveVariants(*wrapper, hostModule, waveW);
                if (more && !waveKernel) {
                    waveKernel = true;
                    forceLoopVectorWidth(latchBr, waveW);
                }
            }

            // The width IS W in a vectorized wave kernel, so width() is rewritten
            // to the constant before vectorizing and folds cleanly.
            if (waveKernel) {
                rewriteWaveWidth(*wrapper, waveW);
                waveMaskAsData(*wrapper, hostModule, waveW);
                if (getenv("CAJETA_XPU_DEBUG_WAVE"))
                    fprintf(stderr, "[wave-pre]\n");
                maybeDumpWaveWrapper(*wrapper, waveW);
            }

            // CAJETA_XPU_CPU_DUMP_PREOPT=<dir>: the module before vectorize,
            // on this path too (the fission path had it first).
            if (const char* dumpDir = std::getenv("CAJETA_XPU_CPU_DUMP_PREOPT")) {
                std::error_code ec;
                llvm::raw_fd_ostream os(
                    std::string(dumpDir) + "/" + wrapper->getName().str()
                        + ".preopt.ll", ec);
                if (!ec) hostModule.print(os, nullptr);
            }
            {
                std::unique_ptr<llvm::DiagnosticHandler> saved;
                const bool dbg = getenv("CAJETA_XPU_DEBUG_WAVE") && waveKernel;
                if (dbg) {
                    saved = ctx.getDiagnosticHandler();
                    ctx.setDiagnosticHandler(std::make_unique<WaveRemarkHandler>());
                    fprintf(stderr, "[wave-remarks] %s\n", entryName.c_str());
                }
                if (waveKernel) markWorkItemLoopsParallel(*wrapper);
                vectorizeFunction(*wrapper, hostTm.get());
                if (dbg) ctx.setDiagnosticHandler(std::move(saved));
            }

            // Fold the substituted variant calls in; no inliner runs at -O0.
            if (waveKernel) {
                std::string op;
                if (!cpuVectorizeDisabled() && waveOpLeftScalar(*wrapper, &op)) {
                    fprintf(stderr,
                            "cajeta: note: [xpu-kernel-skipped] %s: no cpu device "
                            "code: %s was left scalar (the work-item loop did not "
                            "vectorize at the wave width %u), which would run the "
                            "wave op with width-1 semantics\n",
                            entryName.c_str(), op.c_str(), waveW);
                    wrapper->eraseFromParent();
                    if (linked->use_empty()) linked->eraseFromParent();
                    continue;                         // host-stub fallback
                }
                foldWaveVariants(*wrapper);
                maybeDumpWaveWrapper(*wrapper, waveW);
            }
            }   // end of the barrier-free single-loop wrapper build

            // --- Uniform launcher thunk → the per-block wrapper -------------
            // void __cajeta_xpu_cpu_launch.<name>(ptr argv, ptr coord), called ONCE PER
            // BLOCK: of coord's 12 i32s it takes ctaid[3..5], ntid[6..8], nctaid[9..11].
            llvm::FunctionType* thunkTy =
                llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
            llvm::Function* thunk = llvm::Function::Create(
                thunkTy, llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_launch." + symSuffix(method), hostModule);
            thunk->getArg(0)->setName("argv");
            thunk->getArg(1)->setName("coord");
            llvm::Value* argvArg = thunk->getArg(0);
            llvm::Value* coordArg = thunk->getArg(1);

            // A bindless Buffer<T>[] param passes the [count, h…] slot POINTER.
            auto pinfo = collectKernelParamInfo(method, ctx,
                                                hostModule.getDataLayout());
            llvm::BasicBlock* tb = llvm::BasicBlock::Create(ctx, "entry", thunk);
            b.SetInsertPoint(tb);
            std::vector<llvm::Value*> callArgs;
            callArgs.reserve(nReal + kNumBlockCoordParams);
            for (unsigned i = 0; i < nReal; ++i) {
                llvm::Value* slotPtr = b.CreateInBoundsGEP(
                    ptrTy, argvArg, llvm::ConstantInt::get(i64, i), "argv.slot");
                llvm::Value* slot = b.CreateLoad(ptrTy, slotPtr, "argv.ptr");
                if (i < pinfo.size() &&
                    pinfo[i].kind == KernelParamInfo::BufferArray) {
                    // argv[i] already points at [i64 count, i64 h0 …].
                    callArgs.push_back(slot);
                } else {
                    callArgs.push_back(
                        b.CreateLoad(kfnTy->getParamType(i), slot, "arg"));
                }
            }
            for (unsigned j = 3; j < kNumCoordParams; ++j) {   // ctaid+ntid+nctaid xyz
                llvm::Value* cPtr = b.CreateInBoundsGEP(
                    i32, coordArg, llvm::ConstantInt::get(i64, j), "coord.slot");
                callArgs.push_back(b.CreateLoad(i32, cPtr, "coord.val"));
            }
            // coord[12]: the dynamic shared-memory byte count, the trailing param.
            llvm::Value* dynPtr = b.CreateInBoundsGEP(
                i32, coordArg, llvm::ConstantInt::get(i64, kNumCoordParams),
                "coord.dyn.slot");
            callArgs.push_back(b.CreateLoad(i32, dynPtr, "coord.dyn"));
            b.CreateCall(wrapper, callArgs);
            b.CreateRetVoid();

            // ctor: __cajeta_xpu_register_cpu_kernel(entryName, &launchThunk)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_cpu_reg_ctor." + symSuffix(method), hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.cpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, thunk});
            emitManifestRegistration(hostModule, b, nameStr, /*CAJ_XPU_CPU=*/3,
                                     "", manifest);
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            if (manifests) manifests->push_back(manifest);
            ++emitted;
        }
        return emitted;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
