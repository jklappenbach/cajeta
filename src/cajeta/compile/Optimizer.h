// LLVM optimization pipeline helpers. Cajeta's codegen path otherwise runs ONLY
// instruction selection, with no IR optimization over generated user code.

#pragma once

#include "CompilerMode.h"   // OptLevel

namespace llvm {
    class Module;
    class Function;
    class TargetMachine;
}

namespace cajeta {

    // Run the standard per-module pipeline at `level` over `m`; a no-op at O0.
    // `tm` may be null, but O2/O3 vectorization cannot cost-model without it.
    void optimizeModule(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level);

    // Run the ThinLTO PRE-LINK half at `level`: local optimization only, plus
    // AlwaysInliner even at O0. Pair with a bitcode-with-summary write.
    void optimizeModuleThinLTOPreLink(llvm::Module& m, llvm::TargetMachine* tm, OptLevel level);

    // Run the O2 function-simplification pipeline over one function — the "fusion"
    // a Tier-A Jit(f) applies to a specialized body. `tm` may be null here.
    void fuseFunction(llvm::Function& f, llvm::TargetMachine* tm);

    // Vectorize a single function, always and independent of `--opt`: the CPU
    // backend turns its per-block kernel wrapper's work-item loop into SIMD.
    //
    // `scalarizeVectorValues` is for a WAVE kernel wrapper and nothing else.
    // LoopVectorize refuses a loop holding an instruction whose result type is
    // not a valid vector ELEMENT type, and a fixed vector type is not one, so a
    // single cajeta `Vector<T,N>` value (vload, asWords, asBytes, dotAccum) in
    // the region refuses the whole work-item loop with "instruction return type
    // cannot be vectorized" and the wave op is left scalar. Breaking those
    // values into scalars first lets the work-item loop widen at the wave
    // width, and the wave op is what the lanes are FOR. An ordinary
    // data-parallel cpu kernel keeps its vector values, where a
    // `Vector<float32,4>` is deliberate host SIMD.
    void vectorizeFunction(llvm::Function& f, llvm::TargetMachine* tm,
                           bool scalarizeVectorValues = false);

} // namespace cajeta
