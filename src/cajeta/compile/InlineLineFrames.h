// AOT-only expansion of the line-info shadow-stack calls into inline IR.

#pragma once

namespace llvm {
    class Module;
    class TargetMachine;
}

namespace cajeta {

    /** Replaces calls to __cajeta_line_enter/mark/leave in `m` with inline pushes,
     *  marks and pops on __cajeta_shadow_cur; a no-op when CAJETA_INLINE_LINE_FRAMES=0. */
    void inlineLineFrames(llvm::Module& m);

    /** Declares `m`'s line-frame calls as touching only the runtime's memory, so they stay
     *  calls through the optimizer; false (and nothing changed) when `m` must expand early. */
    bool deferLineFrames(llvm::Module& m);

    /** After the optimizer: expands the line-frame calls, one stack fetch per function and one
     *  top update per run of enters or of leaves in a block; re-simplifies merged functions. */
    void coalesceLineFrames(llvm::Module& m, llvm::TargetMachine* tm);

}
