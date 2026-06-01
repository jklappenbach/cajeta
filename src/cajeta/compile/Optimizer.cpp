//
// Optimization pipeline helpers — see header.
//

#include "Optimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/TargetMachine.h"

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
    if (level == OptLevel::O0) return;   // unoptimized by default
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
