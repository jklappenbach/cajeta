// Optimization pipeline helpers - see header.
#include "Optimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"

#include "llvm/Support/CommandLine.h"

namespace cajeta {

namespace {

// PARKED probe: forces LLVM's IPSCCP function specialization, which devirtualizes a constant
// non-capturing closure argument. Unused - call it at the top of optimizeModule() (non-LTO
// only) to re-enable; ThinLTO runs funcspec in the ld.lld backend, where it does not fire.
[[maybe_unused]] void tuneFunctionSpecialization() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& opts = llvm::cl::getRegisteredOptions();
    auto setBool = [&](const char* name, bool v) {
        auto it = opts.find(name);
        if (it != opts.end())
            static_cast<llvm::cl::opt<bool>*>(it->second)->setValue(v);
    };
    auto setUInt = [&](const char* name, unsigned v) {
        auto it = opts.find(name);
        if (it != opts.end())
            static_cast<llvm::cl::opt<unsigned>*>(it->second)->setValue(v);
    };
    setBool("force-specialization", true);
    setBool("funcspec-for-literal-constant", true);
    setUInt("funcspec-min-function-size", 1);
    setUInt("funcspec-max-clones", 8);
}

// Does `f` hold a fixed-vector value anywhere? The scalarize prefix has
// nothing to do when it does not, and running it anyway is not free: the
// InstCombine it used to carry after mem2reg made LoopVectorize PREDICATE and
// scalarize the wave call in a barrier kernel that had no vector value at all
// (XpuCpuBarrierExecTests.waveReduceWithBarrierBlockSum answered 3968 instead
// of 32640 -- one lane per wave, the width-1 identity, measured 2026-09-25).
// Scope is the cheapest correctness margin available here: a kernel with
// nothing to scalarize sees the pipeline it saw before.
bool holdsVectorValue(llvm::Function& f) {
    for (auto& bb : f)
        for (auto& in : bb) {
            if (in.getType()->isVectorTy()) return true;
            for (llvm::Value* op : in.operands())
                if (op->getType()->isVectorTy()) return true;
        }
    return false;
}

// Lower the two scalar<->vector bitcast idioms cajeta's kernel lowering emits
// for `Vector.asWords` / `Vector.asBytes` and for `dotAccum`'s per-lane seam.
//
// The Scalarizer breaks ELEMENTWISE vector ops into lanes, but a bitcast
// between a small integer vector and an integer of the same width is not
// elementwise, so it survives -- and one surviving `<4 x i8>` value is all
// LoopVectorize needs to refuse the whole work-item loop ("instruction return
// type cannot be vectorized"). Measured 2026-09-25: after two Scalarizer plus
// InstCombine rounds the q4k quant shape still carried 80 vector-typed
// instructions in its wrapper, every one of them from these two idioms.
//
// Both rewrites are the little-endian identity, so they are guarded on the
// DataLayout rather than assumed:
//   extractelement (bitcast iN %x to <K x iM>), C   ->  trunc (lshr %x, M*C)
//   bitcast (insertelement chain) to iN             ->  or of shl of zext
// A lane the insertelement chain never sets reads poison or zero from the
// chain's base, and 0 refines both.
bool lowerSmallVectorCasts(llvm::Function& f) {
    const llvm::DataLayout& dl = f.getParent()->getDataLayout();
    if (!dl.isLittleEndian()) return false;
    bool changed = false;
    llvm::SmallVector<llvm::BitCastInst*, 32> work;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(&in))
                work.push_back(bc);
    for (llvm::BitCastInst* bc : work) {
        llvm::Type* srcTy = bc->getSrcTy();
        llvm::Type* dstTy = bc->getDestTy();

        // iN -> <K x iM>, read back only by constant-index extractelement.
        if (srcTy->isIntegerTy()) {
            auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(dstTy);
            if (!vt || !vt->getElementType()->isIntegerTy()) continue;
            llvm::SmallVector<llvm::ExtractElementInst*, 8> reads;
            bool ok = !bc->use_empty();
            for (llvm::User* u : bc->users()) {
                auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(u);
                if (!ee || !llvm::isa<llvm::ConstantInt>(ee->getIndexOperand())) {
                    ok = false; break;
                }
                reads.push_back(ee);
            }
            if (!ok) continue;
            const unsigned m = vt->getElementType()->getIntegerBitWidth();
            llvm::Value* whole = bc->getOperand(0);
            for (llvm::ExtractElementInst* ee : reads) {
                const uint64_t idx = llvm::cast<llvm::ConstantInt>(
                    ee->getIndexOperand())->getZExtValue();
                llvm::IRBuilder<> b(ee);
                llvm::Value* v = whole;
                if (idx)
                    v = b.CreateLShr(v, llvm::ConstantInt::get(srcTy, idx * m));
                ee->replaceAllUsesWith(b.CreateTrunc(v, vt->getElementType()));
                ee->eraseFromParent();
            }
            bc->eraseFromParent();
            changed = true;
            continue;
        }

        // <K x iM> -> iN, built by an insertelement chain off poison/zero.
        auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(srcTy);
        if (!vt || !vt->getElementType()->isIntegerTy() || !dstTy->isIntegerTy())
            continue;
        const unsigned k = vt->getNumElements();
        const unsigned m = vt->getElementType()->getIntegerBitWidth();
        llvm::SmallVector<llvm::Value*, 8> lanes(k, nullptr);
        llvm::Value* cur = bc->getOperand(0);
        bool ok = true;
        while (auto* ie = llvm::dyn_cast<llvm::InsertElementInst>(cur)) {
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2));
            if (!ci || ci->getZExtValue() >= k) { ok = false; break; }
            const unsigned i = static_cast<unsigned>(ci->getZExtValue());
            if (!lanes[i]) lanes[i] = ie->getOperand(1);   // a later insert wins
            cur = ie->getOperand(0);
        }
        if (!ok) continue;
        if (!llvm::isa<llvm::PoisonValue>(cur) && !llvm::isa<llvm::UndefValue>(cur)
            && !llvm::isa<llvm::ConstantAggregateZero>(cur))
            continue;
        llvm::Value* chain = bc->getOperand(0);
        llvm::IRBuilder<> b(bc);
        llvm::Value* acc = llvm::ConstantInt::get(dstTy, 0);
        for (unsigned i = 0; i < k; ++i) {
            if (!lanes[i]) continue;
            llvm::Value* z = b.CreateZExt(lanes[i], dstTy);
            if (i)
                z = b.CreateShl(z, llvm::ConstantInt::get(
                                       dstTy, static_cast<uint64_t>(i) * m));
            acc = b.CreateOr(acc, z);
        }
        bc->replaceAllUsesWith(acc);
        bc->eraseFromParent();
        if (auto* ci = llvm::dyn_cast<llvm::Instruction>(chain))
            llvm::RecursivelyDeleteTriviallyDeadInstructions(ci);
        changed = true;
    }
    return changed;
}

// The helper above as a function pass, so it can sit between the Scalarizer
// rounds in vectorizeFunction's pipeline.
struct SmallVectorCastLoweringPass
    : llvm::PassInfoMixin<SmallVectorCastLoweringPass> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager&) {
        return lowerSmallVectorCasts(f) ? llvm::PreservedAnalyses::none()
                                        : llvm::PreservedAnalyses::all();
    }
};

// Build + cross-register the four analysis managers a new-PM run needs.
struct PassEnv {
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    explicit PassEnv(llvm::TargetMachine* tm) : pb(tm) {
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
    }
};

} // namespace

void optimizeModule(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level) {
    if (level == OptLevel::O0) {
        // Still honor `alwaysinline` at O0: it is an attribute, not a transform, and takes
        // effect only when an AlwaysInlinerPass runs, which the O0 pipeline otherwise skips.
        PassEnv env(tm);
        llvm::ModulePassManager mpm;
        mpm.addPass(llvm::AlwaysInlinerPass());
        mpm.run(m, env.mam);
        return;
    }
    llvm::OptimizationLevel lv;
    switch (level) {
        case OptLevel::O1: lv = llvm::OptimizationLevel::O1; break;
        case OptLevel::O2: lv = llvm::OptimizationLevel::O2; break;
        case OptLevel::O3: lv = llvm::OptimizationLevel::O3; break;
        default:           return;
    }
    PassEnv env(tm);
    llvm::ModulePassManager mpm = env.pb.buildPerModuleDefaultPipeline(lv);
    mpm.run(m, env.mam);
}

void optimizeModuleThinLTOPreLink(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level) {
    if (level == OptLevel::O0) {
        // Same as optimizeModule's O0 branch: run only the AlwaysInlinerPass.
        PassEnv env(tm);
        llvm::ModulePassManager mpm;
        mpm.addPass(llvm::AlwaysInlinerPass());
        mpm.run(m, env.mam);
        return;
    }
    llvm::OptimizationLevel lv;
    switch (level) {
        case OptLevel::O1: lv = llvm::OptimizationLevel::O1; break;
        case OptLevel::O2: lv = llvm::OptimizationLevel::O2; break;
        case OptLevel::O3: lv = llvm::OptimizationLevel::O3; break;
        default:           return;
    }
    PassEnv env(tm);
    // Pre-link half: optimize locally but leave import and cross-module inlining to the
    // linker's ThinLTO backend; optimizing fully here strips symbols the importer needs.
    llvm::ModulePassManager mpm = env.pb.buildThinLTOPreLinkDefaultPipeline(lv);
    mpm.run(m, env.mam);
}

void fuseFunction(llvm::Function& f, llvm::TargetMachine* tm) {
    if (f.isDeclaration()) return;
    PassEnv env(tm);
    llvm::FunctionPassManager fpm = env.pb.buildFunctionSimplificationPipeline(
        llvm::OptimizationLevel::O2, llvm::ThinOrFullLTOPhase::None);
    fpm.run(f, env.fam);
}

void vectorizeFunction(llvm::Function& f, llvm::TargetMachine* tm,
                       bool scalarizeVectorValues) {
    if (f.isDeclaration()) return;
    PassEnv env(tm);

    // Nothing to scalarize, nothing to do: a wave kernel with no vector value
    // keeps exactly the pipeline it had before this parameter existed.
    if (scalarizeVectorValues && !holdsVectorValue(f)) scalarizeVectorValues = false;

    llvm::FunctionPassManager fpm;
    // A cajeta Vector value refuses the whole work-item loop: LoopVectorize's
    // canVectorizeInstrs rejects any instruction whose result type is not a
    // valid vector ELEMENT type, and a fixed vector type is not, so `<4 x
    // float>` cannot widen to the wave width and the remark is "instruction
    // return type cannot be vectorized". ONE vload<4> feeding one
    // Wave.reduceSumF32, with no loop in the kernel at all, is enough to be
    // refused (measured 2026-09-25 with CAJETA_XPU_DEBUG_WAVE=1); the same
    // kernel with scalar loads registers. Scalarizing first turns the vector
    // value into lanes the work-item loop can widen over.
    //
    // TWO rounds, measured: the first leaves the `bitcast i32 to <4 x i8>`
    // chains asWords and asBytes emit, and InstCombine has to fold those
    // before the second round can finish the job.
    const llvm::ScalarizerPassOptions scalarOpts = [] {
        llvm::ScalarizerPassOptions o;
        o.ScalarizeLoadStore = true;
        return o;
    }();
    if (scalarizeVectorValues) {
        fpm.addPass(llvm::ScalarizerPass(scalarOpts));
        fpm.addPass(llvm::InstCombinePass());
        fpm.addPass(SmallVectorCastLoweringPass());
        fpm.addPass(llvm::InstCombinePass());
        fpm.addPass(llvm::ScalarizerPass(scalarOpts));
        fpm.addPass(llvm::InstCombinePass());
    }
    // SROA before mem2reg, and it is a SOUNDNESS prerequisite for the
    // parallel work-item loop, not a tuning: a distributed cooperative-matrix
    // tile keeps its per-lane slots in small constant-indexed array allocas
    // at loop-INVARIANT addresses. Under llvm.loop.parallel_accesses
    // LoopVectorize widens a store to such an address as a uniform store
    // (last lane wins) and the load as a broadcast, so 32 lanes share one
    // slot and q6kWmmaDeqMw4Kernel computed garbage (measured 2026-09-23,
    // cycle 8: registered and wrong; with SROA, cycle 6: 0.23 of the 3% host
    // bar). Scalar replacement puts each lane's slot in its own SSA value.
    fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    fpm.addPass(llvm::PromotePass());                 // mem2reg → SSA
    // After scalarizing, mem2reg promotes a per-work-item local slot into a
    // loop-carried PHI that LoopVectorize reports as "value that could not be
    // identified as reduction is used outside the loop". EarlyCSE clears it.
    //
    // EarlyCSE then InstCombine, and the SCOPE above is what makes the
    // InstCombine safe. In this slot InstCombine makes LoopVectorize predicate
    // and scalarize the wave call -- pred.call.if, one scalar call per lane,
    // the width-1 identity -- on the block-reduce barrier kernel, which holds
    // no vector value and therefore never reaches this branch now. It is also
    // load bearing: q4kWmmaIdMwKernel is the one cajeta-llm kernel this whole
    // change lifts, and with EarlyCSE alone it goes back to being declined
    // (measured 2026-09-25, a full cpu sweep each way). SimplifyCFG here is
    // wrong under any scope: it costs the nested-loop shape a whole vectorized
    // region.
    //
    // The pass bisection was run under opt -mcpu=native over the compiler's
    // own CAJETA_XPU_CPU_DUMP_PREOPT dump. Naming the host CPU is load
    // bearing: without -mcpu the predication does not reproduce at all,
    // because the choice is the vectorizer's cost model. Any future
    // reordering here must be re-measured, not argued.
    if (scalarizeVectorValues) {
        fpm.addPass(llvm::EarlyCSEPass());
        fpm.addPass(llvm::InstCombinePass());
    }
    fpm.addPass(llvm::createFunctionToLoopPassAdaptor(
        llvm::LoopRotatePass()));                     // rotate for LV
    fpm.addPass(llvm::LoopVectorizePass());           // the work-item loop → SIMD
    fpm.addPass(llvm::SLPVectorizerPass());
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.run(f, env.fam);
}

} // namespace cajeta
