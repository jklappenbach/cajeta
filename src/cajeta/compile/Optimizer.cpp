// Optimization pipeline helpers - see header.
#include "Optimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PassTimingInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
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
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "cajeta/xpu/core/DeclarationCheck.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"

#include "llvm/Support/CommandLine.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

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

// LoopVectorize must not SPECULATE a unit stride for a wave kernel. LAA
// versions a loop on "stride == 1" for an index `tid * n` with `n` a runtime
// value, and the loop it leaves for every other `n` is the scalar twin,
// where a wave op is its width-1 stub. Off, the access is a gather. The
// switch is a process-global cl::opt, so it is scoped to the wrapper
// pipeline and the host's own code keeps LLVM's default.
struct ScopedNoUnitStrideSpeculation {
    llvm::cl::opt<bool>* opt = nullptr;
    bool was = true;
    ScopedNoUnitStrideSpeculation() {
        auto& opts = llvm::cl::getRegisteredOptions();
        auto it = opts.find("laa-speculate-unit-stride");
        if (it == opts.end()) return;
        opt = static_cast<llvm::cl::opt<bool>*>(it->second);
        was = opt->getValue();
        opt->setValue(false);
    }
    ~ScopedNoUnitStrideSpeculation() { if (opt) opt->setValue(was); }
};

// Blind the stride analysis to a power-of-two mask on a loop-varying value.
//
// ScalarEvolution reads `x & 15` and `x % 16` as `zext i4 (trunc x)`, and
// when `x` is the work-item induction variable LoopVectorize converts that to
// an affine recurrence under the PREDICATE that the truncation never wraps
// over the loop, which is "block <= 16". The predicate is added by the cost
// model's consecutive-pointer analysis, past the legality threshold, and it
// cannot be refused by option. It becomes a runtime check that sends every
// launch it fails to the scalar twin of the loop, where a wave shuffle is the
// identity and a wave reduce is its own input: `(x & 7) * 4` answered 2 for
// 929, `x & 31` at a two-wave block the same, and fromWords of words loaded
// from a buffer read each lane's own A element for every k (measured
// 2026-10-05, xpu-kernel-independence 4.1.2.4). The same mask written as
// `x - ((x >> k) << k)` is an add of a recurrence and a udiv, which the
// predicate rewriter cannot make affine, so the access is a gather: the right
// answer, with no twin. InstCombine folds the spelling straight back, which
// is why this runs after the last InstCombine before LoopVectorize.
struct BlindLaneMasksPass : llvm::PassInfoMixin<BlindLaneMasksPass> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager&) {
        llvm::SmallVector<llvm::BinaryOperator*, 16> masks;
        for (auto& bb : f)
            for (auto& in : bb) {
                auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&in);
                if (!bo || !bo->getType()->isIntegerTy()) continue;
                auto* c = llvm::dyn_cast<llvm::ConstantInt>(bo->getOperand(1));
                if (!c || !llvm::isa<llvm::Instruction>(bo->getOperand(0))) continue;
                const llvm::APInt& v = c->getValue();
                if (bo->getOpcode() == llvm::Instruction::And) {
                    if (v.isMask() && !v.isZero() && v.countr_one() < v.getBitWidth())
                        masks.push_back(bo);
                } else if (bo->getOpcode() == llvm::Instruction::URem) {
                    if (v.isPowerOf2() && v.logBase2() >= 1) masks.push_back(bo);
                }
            }
        for (auto* bo : masks) {
            auto* c = llvm::cast<llvm::ConstantInt>(bo->getOperand(1));
            const llvm::APInt& v = c->getValue();
            unsigned k = bo->getOpcode() == llvm::Instruction::And ? v.countr_one()
                                                                   : v.logBase2();
            llvm::IRBuilder<> b(bo);
            llvm::Value* x = bo->getOperand(0);
            llvm::Value* hi = b.CreateShl(b.CreateLShr(x, k), k, "lane.hi");
            llvm::Value* lo = b.CreateSub(x, hi, "lane.lo");
            bo->replaceAllUsesWith(lo);
            bo->eraseFromParent();
        }
        return masks.empty() ? llvm::PreservedAnalyses::all()
                             : llvm::PreservedAnalyses::none();
    }
};

// The scalar twin of a widened work-item loop never runs in a wave kernel.
//
// LoopVectorize leaves a scalar copy of every loop it widens, for the
// remainder when the trip count is not a multiple of the width and for every
// launch a runtime check it added turns away. In a wave kernel that copy
// runs the wave ops as their width-1 stubs, which is a wrong answer with no
// diagnostic. After widening, the twin's preheader becomes a call to the
// runtime's `__cajeta_xpu_cpu_scalar_twin(kernel)` and a return from the
// block: the launcher counts it, refuses the launch by name, and
// Device.checkLaunch raises. The twin is then unreachable and deleted.
struct RetireScalarTwinsPass : llvm::PassInfoMixin<RetireScalarTwinsPass> {
    std::string kernelName;
    explicit RetireScalarTwinsPass(llvm::StringRef name) : kernelName(name.str()) {}
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& am);
};

llvm::PreservedAnalyses RetireScalarTwinsPass::run(llvm::Function& f,
                                                   llvm::FunctionAnalysisManager& am) {
    llvm::LoopInfo& li = am.getResult<llvm::LoopAnalysis>(f);
    auto hasHint = [](llvm::Loop* L, llvm::StringRef hint) {
        llvm::MDNode* id = L->getLoopID();
        if (!id) return false;
        for (unsigned i = 1; i < id->getNumOperands(); ++i)
            if (auto* md = llvm::dyn_cast<llvm::MDNode>(id->getOperand(i)))
                if (md->getNumOperands() > 0)
                    if (auto* str = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                        if (str->getString() == hint) return true;
        return false;
    };
    llvm::SmallVector<llvm::Loop*, 4> twins;
    for (llvm::Loop* top : li)
        for (llvm::Loop* L : llvm::depth_first(top)) {
            if (!hasHint(L, "cajeta.xpu.wi") || !hasHint(L, "llvm.loop.isvectorized"))
                continue;
            llvm::BasicBlock* h = L->getHeader();
            if (h->hasName() && h->getName().starts_with("vector.body")) continue;
            // A twin with no wave op in it is ordinary scalar code and stays:
            // a barrier region that only loops over a row has a right answer
            // at width 1, for a block of any size.
            bool wave = false;
            for (llvm::BasicBlock* bb : L->blocks())
                for (auto& in : *bb)
                    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                        if (auto* cf = c->getCalledFunction())
                            if (cf->getName().starts_with("__cajeta_xpu_wave_")
                                && cf->getName() != "__cajeta_xpu_wave_width")
                                wave = true;
            if (wave) twins.push_back(L);
        }
    if (twins.empty()) return llvm::PreservedAnalyses::all();
    llvm::Module& m = *f.getParent();
    llvm::LLVMContext& ctx = f.getContext();
    llvm::FunctionCallee hook = m.getOrInsertFunction(
        "__cajeta_xpu_cpu_scalar_twin",
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                {llvm::PointerType::get(ctx, 0)}, false));
    unsigned retired = 0;
    for (llvm::Loop* L : twins) {
        llvm::BasicBlock* pre = L->getLoopPreheader();
        if (!pre) continue;
        llvm::Instruction* term = pre->getTerminator();
        llvm::IRBuilder<> b(term);
        b.CreateCall(hook, {b.CreateGlobalString(kernelName, "cajeta.twin.kernel")});
        if (f.getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateUnreachable();
        term->eraseFromParent();
        ++retired;
    }
    if (!retired) return llvm::PreservedAnalyses::all();
    llvm::removeUnreachableBlocks(f);
    return llvm::PreservedAnalyses::none();
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

// Rewrite x86 `vpdpbusd` (u8 x s8, four adjacent products into each i32 lane,
// wrapping) as generic IR the Scalarizer can split. A target intrinsic with a
// vector result survives scalarizing and refuses the work-item loop on a VNNI host.
bool expandTargetIntDots(llvm::Function& f) {
    llvm::SmallVector<llvm::CallInst*, 8> work;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                switch (c->getIntrinsicID()) {
                    case llvm::Intrinsic::x86_avx512_vpdpbusd_128:
                    case llvm::Intrinsic::x86_avx512_vpdpbusd_256:
                    case llvm::Intrinsic::x86_avx512_vpdpbusd_512:
                        work.push_back(c);
                        break;
                    default:
                        break;
                }
    for (llvm::CallInst* c : work) {
        llvm::IRBuilder<> b(c);
        llvm::Value* acc = c->getArgOperand(0);
        auto* accTy = llvm::cast<llvm::FixedVectorType>(acc->getType());
        const unsigned n = accTy->getNumElements();
        auto* bytesTy = llvm::FixedVectorType::get(b.getInt8Ty(), n * 4);
        auto* wideTy = llvm::FixedVectorType::get(b.getInt32Ty(), n * 4);
        llvm::Value* u = b.CreateZExt(
            b.CreateBitCast(c->getArgOperand(1), bytesTy), wideTy, "dot.u");
        llvm::Value* s = b.CreateSExt(
            b.CreateBitCast(c->getArgOperand(2), bytesTy), wideTy, "dot.s");
        llvm::Value* prod = b.CreateMul(u, s, "dot.p");
        llvm::Value* sum = acc;
        for (unsigned k = 0; k < 4; ++k) {
            llvm::SmallVector<int, 16> mask(n);
            for (unsigned j = 0; j < n; ++j) mask[j] = static_cast<int>(4 * j + k);
            sum = b.CreateAdd(sum, b.CreateShuffleVector(prod, mask, "dot.k"),
                              "dot.acc");
        }
        c->replaceAllUsesWith(sum);
        c->eraseFromParent();
    }
    return !work.empty();
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
// With CAJETA_TIME_PASSES the run is instrumented and its per-pass timing
// report is printed when the environment is torn down (`ctx` names the
// context the instrumentation attaches to; the report needs one).
struct PassEnv {
    llvm::PassInstrumentationCallbacks pic;
    std::unique_ptr<llvm::StandardInstrumentations> si;
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    explicit PassEnv(llvm::TargetMachine* tm, llvm::LLVMContext* ctx = nullptr)
        : pb(tm, llvm::PipelineTuningOptions(), std::nullopt,
             (ctx && timePassesWanted()) ? &pic : nullptr) {
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        if (ctx && timePassesWanted()) {
            si = std::make_unique<llvm::StandardInstrumentations>(
                *ctx, /*DebugLogging=*/false);
            si->registerCallbacks(pic, &mam);
        }
    }
    ~PassEnv() {
        if (si) si->getTimePasses().print();
    }
};

} // namespace

bool timePassesWanted() {
    // Read every time, not cached: a process that sets the variable, runs a
    // pipeline and unsets it (the tests do) must fall silent again.
    const char* env = std::getenv("CAJETA_TIME_PASSES");
    bool on = env != nullptr && env[0] != 0 && !(env[0] == '0' && env[1] == 0);
    if (on) llvm::TimePassesIsEnabled = true;
    return on;
}

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
    PassEnv env(tm, &m.getContext());
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
    PassEnv env(tm, &m.getContext());
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
                       bool scalarizeVectorValues, llvm::StringRef kernelName) {
    if (f.isDeclaration()) return;
    PassEnv env(tm);

    // Nothing to scalarize, nothing to do: a wave kernel with no vector value
    // keeps exactly the pipeline it had before this parameter existed.
    const bool askedToScalarize = scalarizeVectorValues;
    if (scalarizeVectorValues && !holdsVectorValue(f)) scalarizeVectorValues = false;
    // CAJETA_XPU_CPU_OPT_NOTE=1: one line per wrapper saying whether the
    // scalarize prefix was asked for and whether it ran. Which side of that
    // scope a kernel falls on is the whole question when a kernel stops or
    // starts lowering, and reading it off a 60 MB wave dump is not a way to
    // answer it.
    if (askedToScalarize && std::getenv("CAJETA_XPU_CPU_OPT_NOTE"))
        fprintf(stderr, "[wave-opt] %s: wave kernel, vector values %s\n",
                f.getName().str().c_str(),
                scalarizeVectorValues ? "PRESENT (scalarize prefix runs)"
                                      : "none (prefix skipped)");

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
        expandTargetIntDots(f);
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
    // Only a wave kernel has a twin that must not run; an ordinary kernel's
    // remainder loop is right, and its masks stay as written.
    const bool waveKernel = askedToScalarize;
    if (waveKernel && !xpu::xpuFault("keep-lane-masks"))
        fpm.addPass(BlindLaneMasksPass());
    fpm.addPass(llvm::createFunctionToLoopPassAdaptor(
        llvm::LoopRotatePass()));                     // rotate for LV
    fpm.addPass(llvm::LoopVectorizePass());           // the work-item loop → SIMD
    {
        ScopedNoUnitStrideSpeculation noSpeculation;
        fpm.run(f, env.fam);
    }
    llvm::FunctionPassManager post;
    if (waveKernel)
        post.addPass(RetireScalarTwinsPass(kernelName.empty() ? f.getName() : kernelName));
    post.addPass(llvm::SLPVectorizerPass());
    post.addPass(llvm::InstCombinePass());
    post.addPass(llvm::SimplifyCFGPass());
    post.run(f, env.fam);
}

} // namespace cajeta
