//
// Optimization pipeline helpers — see header.
//

#include "Optimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"

namespace cajeta {

namespace {

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
        // Unoptimized by default — but still honor `alwaysinline`. It is an
        // attribute, not a transform: it only takes effect when an
        // AlwaysInlinerPass actually runs, and the O0 pipeline runs nothing.
        // @ValueType operators (and @Device helpers) are marked alwaysinline so a
        // dispatched value-type operator folds to the same flat IR an intrinsic
        // would; without this they stay real calls at O0/JIT, spilling aggregates
        // through byval/sret and defeating register residency. See
        // plans/value-type-overloading-plan.md (S1b / review fix #1).
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
        // Same rationale as optimizeModule's O0 branch: honor `alwaysinline`
        // (the AlwaysInlinerPass is the only transform), but here it also lets a
        // ThinLTO build at O0 still fold @Inline hot paths once imported.
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
    // Pre-link half: optimize locally but leave cross-module work (import +
    // inlining) for the linker's ThinLTO backend. This is what makes the module
    // summary meaningful — full per-module optimization here would prematurely
    // localize/strip symbols the importer still needs.
    llvm::ModulePassManager mpm = env.pb.buildThinLTOPreLinkDefaultPipeline(lv);
    mpm.run(m, env.mam);
}

void vectorizeFunction(llvm::Function& f, llvm::TargetMachine* tm) {
    if (f.isDeclaration()) return;
    PassEnv env(tm);

    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::PromotePass());                 // mem2reg → SSA
    fpm.addPass(llvm::createFunctionToLoopPassAdaptor(
        llvm::LoopRotatePass()));                     // rotate for LV
    fpm.addPass(llvm::LoopVectorizePass());           // the work-item loop → SIMD
    fpm.addPass(llvm::SLPVectorizerPass());
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.run(f, env.fam);
}

} // namespace cajeta
