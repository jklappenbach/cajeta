//
// CPU kernel registration pass — see header.
//
// For each @Kernel: lower it to a host function in a fresh module that shares
// the host module's LLVMContext (so the two can be linked), decorate the symbol
// to avoid clashing with the kernel's host stub, link it into the host module,
// then append an llvm.global_ctors entry calling
// __cajeta_xpu_register_cpu_kernel(entryName, &fn). A mid-lowering XPU-N01
// throw discards the fresh module, leaving the host module untouched.
//

#include "CpuRegistration.h"
#include <llvm/IR/DiagnosticInfo.h>
#include "CpuKernelLowering.h"
#include "../lowering/KernelLowering.h"   // collectKernelParamInfo / KernelParamInfo
#include "CpuBackend.h"
#include "CpuBarrierFission.h"
#include "cajeta/compile/Optimizer.h"

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"
#include <cctype>
#include <map>

#include "llvm/Analysis/TargetTransformInfo.h"
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

// Debug seam: when CAJETA_XPU_CPU_NO_VECTORIZE is set in the environment, skip
// the work-item LoopVectorize pass on the per-block wrapper. Correctness is
// unaffected (the scalar work-item loops are already complete); this only lets
// tooling/tests inspect the fission output before loop-rotate/vectorize fold
// and rename the per-region loop blocks. Off by default.
bool cpuVectorizeDisabled() {
    return std::getenv("CAJETA_XPU_CPU_NO_VECTORIZE") != nullptr;
}

// --- Increment 5C: wave-op SIMD via the Vector Function ABI ----------------
//
// A wave op lowers (in CpuKernelLowering) to a scalar call to its
// `__cajeta_xpu_wave_*` stub. These helpers give that scalar call a SIMD
// *vector variant* and force the per-block work-item loop to vectorize at the
// host's native vector width W, so LoopVectorize substitutes the variant and
// the wave runs as W SIMD lanes. If W < 2 or the host TM is absent, nothing is
// attached and the scalar (width-1) call runs — always correct.

// The host's native i32 fixed-vector width (16 on AVX-512, 8 on AVX2, 4 on
// SSE2). 0 if no TM. This is the wave width on the CPU backend.
unsigned cpuVectorWidthI32(llvm::TargetMachine* tm, llvm::Function& f) {
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

// Create an empty `internal alwaysinline memory(none)` variant function with
// the given name + signature; the caller fills the body. alwaysinline so
// release builds fold it into the vectorized loop.
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

// One wave op's width-W SIMD variants. The scalar stub gets a
// `vector-function-abi-variant` attribute naming both an unmasked (`_vW`) and a
// masked (`_Mv16`) variant. LoopVectorize uses the unmasked one in uniform code
// and the masked one when the call is in divergent (if-guarded) control flow,
// passing the active-lane predicate as the trailing mask argument — matching
// GPU active-mask semantics.
//
// The variant bodies are synthesized in IR from portable vector ops
// (reduce/select/bitcast/gather), so they legalize to real SIMD on any host.
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

    // The VFABI parameter-token string (one char per scalar parameter); built
    // alongside each op. `_ZGV_LLVM_N<W><tokens>_<scalar>(<unmasked>)` and the
    // `M` (masked) form which appends the predicate.
    std::string tokens;
    llvm::Function* unmasked = nullptr;
    llvm::Function* masked = nullptr;

    if (scalarName == "__cajeta_xpu_wave_reduce_sum_u32") {
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        // <W x i32> v(<W x i32> x) = broadcast(reduce.add(x))
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* s = b.CreateAddReduce(unmasked->getArg(0));
            b.CreateRet(b.CreateVectorSplat(W, s));
        }
        // <W x i32> Mv(<W x i32> x, <W x i1> m) = broadcast(reduce.add(m?x:0))
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
        // <W x i64> v(<W x i1> p) = broadcast(zext(bitcast<W x i1>->iW))
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(rTy, {pTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* bits = b.CreateBitCast(unmasked->getArg(0), iW);
            llvm::Value* z = b.CreateZExt(bits, i64);
            b.CreateRet(b.CreateVectorSplat(W, z));
        }
        // masked: ballot over active lanes only → bitcast(p & m)
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
        // <W x i32> v(<W x i32> val, <W x i32> src): result[i] = val[src[i]] —
        // a per-lane dynamic gather within the wave, unrolled W times.
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
        // masked: same gather; LoopVectorize blends inactive result lanes back
        // to their pre-call value, so the mask need not change the computation.
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
        // The reduce family beyond sum: broadcast(reduce_op(x)). Unsigned min/max
        // (uint32 surface). The masked form folds inactive lanes to the op's
        // identity so they don't perturb the reduction.
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        // (reduce builder, identity for masked-out lanes).
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
    } else if (scalarName.ends_with("_u32_m") &&
               scalarName.starts_with("__cajeta_xpu_wave_reduce_")) {
        // Mask-as-data reduce spellings (value, active). Produced by the
        // waveMaskAsData rewrite below: the guard rides as a data argument, so
        // the call sits in UNCONDITIONAL code and the plain `N` variant — which
        // is exactly the guarded op's masked reduce — always applies. The `M`
        // variant composes an enclosing residual predicate by AND, so a
        // rewritten call that still ends up under an outer divergent guard
        // remains correct through LoopVectorize's masked path too.
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
        // <W x i32> v(<W x i32> x, <W x i1> act) = broadcast(reduce(act?x:id))
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
        // <W x i32> Mv(x, act, pred) = broadcast(reduce((act&pred)?x:id))
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
        // Exclusive prefix scan: result[i] = op over lanes 0..i-1 (lane 0 = id).
        // Unrolled across the W lanes — the running accumulator threads through.
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
        // masked: inactive lanes contribute the identity (no effect on the scan).
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

// `width()` takes no argument, so it cannot carry a VFABI variant (a 0-param
// variant is rejected by the verifier). In a vectorized wave kernel the wave
// width IS W (the architectural width, like a GPU warp size — constant across
// active and inactive lanes), so rewrite each `__cajeta_xpu_wave_width()` call
// to the constant W. Returns the number rewritten.
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

// Force `latch` (a work-item loop's back-edge branch) to vectorize at width W
// via self-referential `!llvm.loop` metadata — so the forced VF equals the
// variant's VF and LoopVectorize substitutes it.
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
    llvm::MDNode* loopId = llvm::MDNode::getDistinct(ctx, ops);
    loopId->replaceOperandWith(0, loopId);
    latch->setMetadata("llvm.loop", loopId);
}

// The wave-op runtime stubs that carry a SIMD VFABI variant. `width()` is handled
// separately (rewritten to the constant W), and lane_id/is_first_lane lower inline.
static const char* const kWaveOps[] = {
    "__cajeta_xpu_wave_reduce_sum_u32",
    "__cajeta_xpu_wave_reduce_max_u32",
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
//
// A wave reduce under divergent control flow (`if (c) { .. Wave.reduceSum(x)
// .. }`) must widen through its masked VFABI variant — but whether
// LoopVectorize picks the variant or SCALARIZES the call per predicated lane
// is a COST decision, and on hosts without masked-memory SIMD (NEON; plain
// SSE2) it picks scalarization. A per-lane scalar wave stub is width-1
// identity, so every active lane silently gets its own value back instead of
// the wave reduction — found as wrong sums on arm64-darwin at the v0.21.0
// gate, reproduced on x86 with CAJETA_XPU_CPU_MCPU=x86-64 MATTR=+sse2.
//
// Correctness must not hang on a cost model, so this pre-pass takes the
// decision away: for a guarded wave reduce in triangle shape
//
//   P:  br i1 c, B, J          P:  br i1 c, B1, M
//   B:  ...pre...              B1: ...pre... ; br M
//       r = reduce(x)     →    M:  xm = phi [x, B1], [poison, P]
//       ...post...                 mask = phi [true/inner, B1], [false, P]
//   J:  ...                        r = reduce_m(xm, mask)   ; UNCONDITIONAL
//                                  br i1 c, B2, J
//                              B2: ...post... ; br J
//
// the call moves to the always-executed merge block M with the guard as a
// DATA argument; only the memory ops stay predicated (per-lane scalarized
// loads/stores are slow but correct — a per-lane scalarized CROSS-LANE op is
// not). The `_m` stub's plain `N` variant is the reduce-with-active-mask
// shell, so the widened form is correct no matter which vectorization
// strategy the memory ops get. Runs to fixpoint: a rewritten `_m` call that
// is itself under an outer guard matches again, composing masks through the
// phi (inner mask on the guarded edge, false on the bypass edge).
//
// Non-triangle shapes (else-arms with code, multi-exit guard blocks) are left
// alone — no worse than today — and noted under CAJETA_XPU_DEBUG_WAVE.


// CAJETA_XPU_DEBUG_WAVE: surface LoopVectorize's own remarks for the wrapper
// being vectorized — in-process there is no -pass-remarks, and "it vectorized
// under opt but not in the pipeline" is undebuggable without them.
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
    if (name.starts_with("__cajeta_xpu_wave_reduce_") && name.ends_with("_u32")) {
        *isMasked = false;
        return true;
    }
    if (name.starts_with("__cajeta_xpu_wave_reduce_") && name.ends_with("_u32_m")) {
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
        // Find an eligible call first (cheap reject for most blocks).
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

        // Triangle guard: unique pred P ending in condbr, unique succ J that
        // is P's other successor.
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

        // Resolve the _m stub BEFORE any surgery. The runtime bitcode DEFINES
        // it (width-1 C fallback, `_Bool` mask → i1), so it may already be in
        // the module; a type mismatch must bail here, not mid-rewrite.
        llvm::Function* calleeF = call->getCalledFunction();
        const std::string mName = wasMasked
            ? calleeF->getName().str()
            : calleeF->getName().str() + "_m";
        auto* mTy = llvm::FunctionType::get(i32, {i32, i1}, false);
        llvm::Function* mf = m.getFunction(mName);
        if (mf && mf->getFunctionType() != mTy) {
            if (getenv("CAJETA_XPU_DEBUG_WAVE"))
                fprintf(stderr, "[wave-msd] %s: unexpected signature on %s — "
                        "left predicated\n",
                        f.getName().str().c_str(), mName.c_str());
            continue;
        }

        // Split: bb keeps the pre-call code (B1); the call and everything
        // after move to B2. splitBasicBlock rewrites succ's phis to B2.
        llvm::BasicBlock* b1 = &bb;
        llvm::BasicBlock* b2 =
            b1->splitBasicBlock(call->getIterator(), b1->getName() + ".wave.tail");

        // Merge block M — every path now runs it.
        llvm::BasicBlock* mblk = llvm::BasicBlock::Create(
            ctx, b1->getName() + ".wave.msd", &f, b2);
        b1->getTerminator()->setSuccessor(0, mblk);          // B1 -> M
        pbr->setSuccessor(onTrue ? 1 : 0, mblk);             // P bypass -> M
        succ->replacePhiUsesWith(pred, mblk);                // J phis: P -> M

        // Lift every B1 value used outside B1 through an M phi (B1 no longer
        // dominates M/B2/J — P's bypass edge enters M directly).
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

        // The lane-active mask: true down the guarded edge (or the inner
        // call's own mask, composing nested guards), false down the bypass.
        auto* mask = llvm::PHINode::Create(i1, 2, "wave.mask", mblk);
        llvm::Value* innerActive = llvm::ConstantInt::getTrue(ctx);
        if (wasMasked) {
            llvm::Value* om = call->getArgOperand(1);
            // The operand may have been rewritten to an M phi above; for the
            // B1 edge we need the value as seen at B1's end.
            if (auto* omPhi = llvm::dyn_cast<llvm::PHINode>(om))
                if (omPhi->getParent() == mblk)
                    om = omPhi->getIncomingValueForBlock(b1);
            innerActive = om;
        }
        mask->addIncoming(innerActive, b1);
        mask->addIncoming(llvm::ConstantInt::getFalse(ctx), pred);

        // The unconditional masked call. The value operand reads through the
        // lifted phi (uses were rewritten above), or a dominating def as-is.
        // A wrong-typed pre-existing _m stub would come back from
        // getOrInsertFunction as a bitcast ConstantExpr — silently skipping
        // the VFABI attach, and a mapping-less call FAILS LoopVectorize's
        // LEGALITY, killing vectorization of the whole wrapper (found via the
        // in-process remark "call instruction cannot be vectorized"). The
        // type was therefore verified before surgery above.
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
        // Match the C definition's `_Bool` ABI: the callee assumes a
        // zero-extended mask register.
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
    // The bound is a backstop; real kernels converge in nesting-depth steps.
    for (unsigned i = 0; i < 64; ++i)
        if (!rewriteOneGuardedWaveCall(f, m, waveW)) break;
}

// Attach SIMD variants for every wave op `f` uses and (if any, or `width()` is
// used) report it a wave kernel. Shared by the single-loop (5C) and barrier
// (fission) paths.

// CAJETA_XPU_DEBUG_WAVE=1: dump each wave kernel's wrapper IR AFTER
// vectorization + variant folding to stderr. The knob exists to diagnose
// per-host LoopVectorize divergence from CI logs alone — the v0.21.0 gate
// found the masked reduceSum variant produced wrong lane sums on
// arm64-darwin while the identical IR was right on arm64-linux, and the
// only way to see what the vectorizer did on a runner is to print it.
static void maybeDumpWaveWrapper(const llvm::Function& f, unsigned waveW) {
    if (!getenv("CAJETA_XPU_DEBUG_WAVE")) return;
    std::string out;
    llvm::raw_string_ostream os(out);
    f.print(os);
    // Also show every wave-stub declaration + its VFABI attr, so a broken or
    // missing mapping is visible next to the wrapper that needed it.
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
            attachWaveVariants(m, op, waveW);
            waveKernel = true;
        }
    if (callsRuntimeFn(f, "__cajeta_xpu_wave_width")) waveKernel = true;
    return waveKernel;
}

// Fold the LoopVectorize-substituted wave variant calls (alwaysinline) into the
// loop so codegen emits the reduce/gather/ballot SIMD directly rather than a
// per-iteration call (no inliner runs at -O0).
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
                               const std::string& /*arch*/) {
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

        // Host TargetMachine — supplies TargetTransformInfo so LoopVectorize can
        // cost-model the per-block wrapper's work-item loop (Inc 5B). One TM for
        // all kernels; null is tolerated (vectorization just won't fire).
        std::unique_ptr<llvm::TargetMachine> hostTm = createCpuTargetMachine();

        // The per-block wrapper takes 9 coordinate params (ctaid.{x,y,z},
        // ntid.{x,y,z}, nctaid.{x,y,z}); the 3 tid coords become its internal
        // work-item loop var. nctaid (grid block-count) rides through so the
        // kernel's gridSize(dim) = nctaid·ntid (grid-stride for-each, Item 6 St.2).
        const unsigned kNumBlockCoordParams = 9;

        // A kernel's LLVM symbols must be unique per PROGRAM, but the runtime
        // registry is keyed by the SIMPLE name (every backend resolves a
        // launch that way). Two classes each declaring `@Kernel dotK` used to
        // emit `__cajeta_xpu_cpu.dotK` twice and fail at link with a
        // duplicate symbol. Qualify the symbols with the declaring class, and
        // diagnose the registry collision the symbols were masking — two
        // kernels registering under one name would otherwise silently
        // dispatch whichever ctor ran last.
        auto symSuffix = [](const cajeta::MethodPtr& m) {
            std::string q = m->getParent() ? m->getParent()->toCanonical()
                                           : std::string();
            std::string full = q.empty() ? m->getName() : q + "." + m->getName();
            for (char& c : full)
                if (!std::isalnum((unsigned char) c) && c != '.' && c != '_')
                    c = '_';
            return full;
        };
        // NOTE: this catches a collision only WITHIN one compilation unit;
        // kernels are emitted per-module, so two files each declaring the
        // same simple name are not visible to each other here. The symbol
        // qualification above is what keeps those from colliding at link;
        // the registry-level shadowing across files remains open.
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

            // Lower into a fresh module sharing the host context (so it can be
            // linked). A throw here destroys `mod` with the host untouched.
            auto mod = std::make_unique<llvm::Module>("xpu.cpu." + entryName, ctx);
            mod->setDataLayout(hostModule.getDataLayout());
            mod->setTargetTriple(hostModule.getTargetTriple());
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, *mod);
            } catch (cajeta::Exception& e) {
                // Unsupported construct (XPU-N01) — this kernel gets NO CPU
                // code, and a @Kernel has no host path to fall back to: the
                // launch finds nothing registered, prints one runtime line,
                // and every output buffer reads back ZERO. Say so at build
                // time, unconditionally, exactly as the amdgpu, nvptx and
                // vulkan backends already do. This was gated behind
                // CAJETA_XPU_DEBUG_LOWER, which made the DEFAULT backend the
                // only one that skipped in silence — and that is how the
                // missing widen/convert ladder read as a working build that
                // computed zeros (plan 8.10, 8.11).
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

            // --- Per-block wrapper (POCL model, Inc 5B) ---------------------
            // void __cajeta_xpu_cpu_block.<name>(real params...,
            //        i32 ctaid.x, ctaid.y, ctaid.z, ntid.x, ntid.y, ntid.z)
            // Loops the work-item index tid.x over [0, ntid.x) calling the
            // per-work-item kernel; the kernel is then inlined and the loop is
            // handed to LoopVectorize → SIMD. 1-D over tid.x (the launch ABI is
            // 1-D: ntid.y/z == 1); tid.y/z are 0.
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
            // The trailing wrapper param is the dynamic shared byte count; a
            // `shared T[runtimeN]` array allocas it per block (else unused → DCE).
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

            // A kernel that calls Barrier.workgroup() can't run as one work-item
            // loop — it is split at each barrier into regions, each looped over
            // the block (Inc 6 fission). XPU-N02 on an unsupported construct →
            // discard + fall back to the host stub.
            // Both paths now run a 3-D work-item loop nest (the non-barrier
            // wrapper directly; fission per region), so a multi-dim block runs on
            // either (2D/3D stage 4).
            if (usesBarrier(*linked)) {
                std::vector<llvm::UncondBrInst*> wiLatches;
                try {
                    std::vector<llvm::Value*> ctaidV = {ctaidX, ctaidY, ctaidZ};
                    std::vector<llvm::Value*> ntidV  = {ntidX,  ntidY,  ntidZ};
                    std::vector<llvm::Value*> nctaidV = {nctaidX, nctaidY, nctaidZ};
                    fissionBarrierKernel(linked, wrapper, nReal, ctaidV, ntidV,
                                         nctaidV, hostModule, &wiLatches,
                                         dynSharedBytes);
                } catch (cajeta::Exception&) {
                    wrapper->eraseFromParent();
                    linked->eraseFromParent();    // has barrier markers; unusable
                    continue;                     // host-stub fallback
                }
                linked->eraseFromParent();        // body cloned into the wrapper

                // Wave + barrier composition (Inc 9): each fission region is a
                // clean counted work-item loop, so vectorize them at width W with
                // the wave SIMD variants exactly as the 5C single-loop path does —
                // a wave op inside a region becomes W SIMD lanes, the barriers
                // delimit regions. Forcing VF=W on every region loop is safe (the
                // block width is uniform across regions); only wave-bearing
                // regions carry the wave-width-divides-block assumption (as in 5C).
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
                if (!cpuVectorizeDisabled())
                    vectorizeFunction(*wrapper, hostTm.get());
                if (waveKernel) {
                    foldWaveVariants(*wrapper);
                    maybeDumpWaveWrapper(*wrapper, waveW);
                }
            } else {
            // 3-D work-item loop nest over (tid.z, tid.y, tid.x). The innermost
            // tid.x loop is the vectorizable/wave loop — so a 1-D block
            // (ntid.y/z == 1) runs identically to before (the z/y loops are
            // single-trip), and a 2-D/3-D block runs the whole nest (2D/3D launch
            // stage 3). The kernel is called once per work-item with its full 3-D
            // tid coordinate.
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

            // --- Wave-op SIMD setup (Inc 5C) -------------------------------
            // If the kernel uses wave ops, give each its VFABI vector variants
            // (unmasked + masked) and force the work-item loop to vectorize at
            // the host's native width W (the CPU wave width) so LoopVectorize
            // substitutes the SIMD variant — the wave becomes W SIMD lanes.
            // Attribute + variant + loop-width metadata go in BEFORE inlining;
            // the loop metadata rides along when InlineFunction moves the latch.
            const unsigned waveW = cpuVectorWidthI32(hostTm.get(), *wrapper);
            bool waveKernel = false;
            if (waveW >= 2) {
                waveKernel = setupWaveVariants(*linked, hostModule, waveW);
                if (waveKernel) forceLoopVectorWidth(latchBr, waveW);
            }

            // Inline the per-work-item kernel into the loop body, then vectorize
            // the loop (mem2reg + LoopVectorize). This is what turns the CPU
            // backend's wave from width-1 to native SIMD width.
            llvm::InlineFunctionInfo ifi;
            llvm::InlineFunction(*kcall, ifi);

            // The wave width is W in a vectorized wave kernel (architectural,
            // like a warp size) — rewrite width() calls to the constant before
            // vectorizing so the value is uniform and folds cleanly.
            if (waveKernel) {
                rewriteWaveWidth(*wrapper, waveW);
                waveMaskAsData(*wrapper, hostModule, waveW);
                if (getenv("CAJETA_XPU_DEBUG_WAVE"))
                    fprintf(stderr, "[wave-pre]\n");
                maybeDumpWaveWrapper(*wrapper, waveW);
            }

            {
                std::unique_ptr<llvm::DiagnosticHandler> saved;
                const bool dbg = getenv("CAJETA_XPU_DEBUG_WAVE") && waveKernel;
                if (dbg) {
                    saved = ctx.getDiagnosticHandler();
                    ctx.setDiagnosticHandler(std::make_unique<WaveRemarkHandler>());
                }
                vectorizeFunction(*wrapper, hostTm.get());
                if (dbg) ctx.setDiagnosticHandler(std::move(saved));
            }

            // Fold the now-substituted `_vW`/`_Mv16` wave variant calls into the
            // loop (they are alwaysinline, but no inliner runs at -O0) so codegen
            // emits the reduce/gather/ballot SIMD directly rather than a
            // per-iteration call.
            if (waveKernel) {
                foldWaveVariants(*wrapper);
                maybeDumpWaveWrapper(*wrapper, waveW);
            }
            }   // end of the barrier-free single-loop wrapper build

            // --- Uniform launcher thunk → the per-block wrapper -------------
            // void __cajeta_xpu_cpu_launch.<name>(ptr argv, ptr coord). The
            // runtime calls it ONCE PER BLOCK; coord carries 12 i32s, but the
            // wrapper consumes only ctaid.{x,y,z} (coord[3..5]), ntid.{x,y,z}
            // (coord[6..8]) and nctaid.{x,y,z} (coord[9..11]) — the work-item loop
            // lives in the wrapper. argv[i] points to the i-th real argument's
            // value (the cuLaunch/hipModuleLaunch kernelParams convention).
            llvm::FunctionType* thunkTy =
                llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
            llvm::Function* thunk = llvm::Function::Create(
                thunkTy, llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_launch." + symSuffix(method), hostModule);
            thunk->getArg(0)->setName("argv");
            thunk->getArg(1)->setName("coord");
            llvm::Value* argvArg = thunk->getArg(0);
            llvm::Value* coordArg = thunk->getArg(1);

            // Param kinds — a bindless Buffer<T>[] param is passed as the
            // [count, h…] slot POINTER itself (the device default
            // bufferArrayElement loads handles out of it), unlike a normal
            // arg whose value is loaded FROM the slot.
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
                    // argv[i] already points at [i64 count, i64 h0 …]; pass it
                    // straight through (the kernel's ptr param IS that array).
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
            // coord[12]: the dynamic shared-memory byte count (the launch's
            // `sharedBytes:`), passed as the wrapper's trailing param.
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
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
