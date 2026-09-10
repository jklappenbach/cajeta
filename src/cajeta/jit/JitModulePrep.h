// Per-module preparation shared by every ORC delivery path. Run BOTH passes over
// every module before verifying ANY of them: a cross-module use from module B
// trips module A's verifier even though the fix belongs to B.
#pragma once

namespace llvm { class Module; }

namespace cajeta::jit {

    // Make a module SELF-CONTAINED: every use of a GlobalValue homed elsewhere is
    // rewritten to a same-named declaration here, which ORC resolves by name.
    // Constants are context-uniqued, so trees are REBUILT through ValueMapper.
    void legalizeCrossModuleRefs(llvm::Module* m);

    // Demote every instantiation-mangled definition (its name carries '<') to
    // weak_odr, since two strong definitions of one instantiation fail addIRModule
    // outright. Returns the count demoted, so a caller can assert the pass fired.
    int demoteInstantiationsToWeakODR(llvm::Module* m);

}  // namespace cajeta::jit
