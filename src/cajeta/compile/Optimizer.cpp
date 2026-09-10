// Optimization pipeline helpers - see header.
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
