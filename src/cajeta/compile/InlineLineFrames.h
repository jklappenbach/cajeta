// AOT-only expansion of the line-info shadow-stack calls into inline IR.

#pragma once

namespace llvm {
    class Module;
}

namespace cajeta {

    /** Replaces calls to __cajeta_line_enter/mark/leave in `m` with inline pushes,
     *  marks and pops on __cajeta_shadow_cur; a no-op when CAJETA_INLINE_LINE_FRAMES=0. */
    void inlineLineFrames(llvm::Module& m);

}
