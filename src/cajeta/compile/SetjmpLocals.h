// Keeps in memory the locals a longjmp back to a setjmp would otherwise read stale.

#pragma once

namespace llvm {
    class Module;
}

namespace cajeta {

    /** Keeps in memory (volatile accesses) each alloca a try body stores and the path from its
     *  setjmp's second return reads; names unnamed ones `setjmp.pinned`. */
    void pinSetjmpLocals(llvm::Module& m);

}
