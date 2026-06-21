//
// LLVM optimization pipeline helpers (CajetaXPU Increment 5B).
//
// Cajeta's codegen path runs ONLY instruction selection (addPassesToEmitFile) —
// no IR optimization on generated user code. This adds two reusable entry
// points: a general per-module pipeline (`--opt`) and a focused per-function
// vectorization pass used to turn the CPU backend's per-block kernel wrapper
// into SIMD regardless of `--opt`. Both are built on the new PassBuilder API,
// mirroring the mem2reg setup in the NVPTX/AMDGPU backends.
//

#pragma once

#include "CompilerMode.h"   // OptLevel

namespace llvm {
    class Module;
    class Function;
    class TargetMachine;
}

namespace cajeta {

    // Run the standard per-module optimization pipeline at `level` over `m`.
    // No-op at O0 (preserves the unoptimized-by-default behavior). O2/O3 include
    // LoopVectorize + SLPVectorizer. `tm` supplies TargetTransformInfo (may be
    // null, but vectorization needs it to cost-model — pass the host TM).
    void optimizeModule(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level);

    // Run the ThinLTO PRE-LINK pipeline at `level` over `m` — the per-module half
    // of ThinLTO. It optimizes locally but stops short of transforms that must
    // wait for the cross-module import decisions made at link time (the lld
    // ThinLTO backend runs the post-link pipeline + cross-module inlining). At O0
    // it still runs AlwaysInliner (so `@Inline`/@ValueType take effect). Pair with
    // a ThinLTO-bitcode-with-summary write so the linker can import across modules.
    void optimizeModuleThinLTOPreLink(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level);

    // Run a focused vectorization pipeline on a single function: mem2reg,
    // loop-rotate, LoopVectorize, SLPVectorizer, instcombine, simplifycfg. Used
    // by the CPU backend to vectorize the per-block kernel wrapper's work-item
    // loop into SIMD — always, independent of `--opt`. `tm` (host) supplies TTI.
    void vectorizeFunction(llvm::Function& f, llvm::TargetMachine* tm);

} // namespace cajeta
