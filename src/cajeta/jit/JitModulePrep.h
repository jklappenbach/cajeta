//
// Per-module preparation shared by every ORC delivery path.
//
// Both passes below were file-local to CajetaJitHost.cpp, which was fine while
// the JIT host was the only consumer. The kernel session (jupyter-kernel U1)
// delivers a module PER CELL into its own JITDylib and needs exactly the same
// preparation, so they live here rather than being duplicated or made public
// by accident.
//
// Run BOTH over every module before verifying ANY of them: a cross-module use
// from module B trips module A's verifier even though the fix belongs to B.
//
#pragma once

namespace llvm { class Module; }

namespace cajeta::jit {

    // Make a module SELF-CONTAINED. Instantiation emission can leave operands
    // pointing at GlobalValues homed in another module (an Optional<UserType>
    // method emitted into the user module still referencing the stdlib
    // module's __cajeta_alloc, a sibling method, or a #VTable object). A
    // whole-program `Linker::linkModules` merge legalized these implicitly;
    // per-module ORC delivery has no merge, so every foreign GlobalValue use
    // is rewritten to a same-named DECLARATION in this module and ORC
    // resolves it by name at materialization.
    //
    // Constants are uniqued per LLVMContext (shared across modules), so
    // replacement REBUILDS constant trees through ValueMapper rather than
    // mutating in place.
    void legalizeCrossModuleRefs(llvm::Module* m);

    // Template instantiations are ODR. Two strong definitions of one
    // instantiation in a single JITDylib fail `addIRModule` outright
    // ("duplicate definition"), which happens whenever a retained module and
    // a freshly emitted one both carry the symbol — under residency, and
    // between two notebook cells that use the same specialization. Demote
    // every instantiation-mangled definition (the name carries '<') to
    // weak_odr so ORC picks one; non-template symbols keep their linkage.
    //
    // Returns the number of definitions demoted, so a caller can assert the
    // pass actually fired rather than merely that nothing exploded.
    int demoteInstantiationsToWeakODR(llvm::Module* m);

}  // namespace cajeta::jit
