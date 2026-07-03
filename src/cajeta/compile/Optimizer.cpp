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

#include "llvm/Support/CommandLine.h"

namespace cajeta {

namespace {

// PARKED EXPERIMENT (closure-devirt via LLVM function specialization). Forcing
// IPSCCP function-specialization devirtualizes a generic callee's constant
// non-capturing closure argument (the comparator) -> direct, inlinable call.
// VALIDATED the ceiling in the NON-LTO pipeline: sort-int64 ascending
// 0.78->0.21ms (beats std::sort), random 2.80->1.94ms. LIMITATIONS that drove
// us to build Cajeta IR instead: (1) narrow — LLVM's cost model fired only for
// `sort`, not binarySearch/streams; (2) version-fragile `force-specialization`
// debug flag; (3) ThinLTO conflict — funcspec runs in the ld.lld backend
// cross-module where it doesn't fire, and a pre-link IPSCCP breaks the link
// (private-symbol/summary desync). Kept here as a reference probe; the real
// solution is deterministic, total specialization in CIR. See
// specs/archive/cajeta-ir-spec.md §1.2.
//
// TO RE-ENABLE (the validated non-LTO probe): call tuneFunctionSpecialization()
// at the top of optimizeModule() (the O1/O2/O3 path). For ThinLTO the backend
// runs in ld.lld, so the equivalent attempt was passing the same flags via
// `-Wl,-mllvm,-force-specialization` (+ -funcspec-for-literal-constant=true,
// -funcspec-min-function-size=1, -funcspec-max-clones=8) in Compiler.cpp's
// ThinLTO link args — which did NOT devirtualize cross-module (limitation 3).
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
