#pragma once

// lazy-codegen 2.2.3 — generate one method's body and package it for ORC.
//
// Follows the kernel's delivery discipline: the front-end keeps owning its
// llvm::Module (later cells keep mutating it), so ORC only ever receives a
// bitcode SNAPSHOT reparsed into its own context. Here the snapshot holds one
// definition: the requested method's body, everything else a declaration whose
// materialization cascades back through the generator (spec 3.4).
//
// Call with the CompilerGate held — generateCode() touches the compiler's
// process-wide state. CajetaDefinitionGenerator::tryToGenerate already does.

#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"

#include <set>
#include <string>

namespace llvm { class GlobalValue; class Module; }

namespace cajeta {

    llvm::Expected<llvm::orc::ThreadSafeModule>
    emitMethodModule(const MethodPtr& method);

    // A definition codegen synthesized outside the method table — a drop
    // thunk, a vtable or #ClassObject global — snapshotted as-is; no
    // generateCode, it already exists in a live module.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    snapshotLiveDefinition(llvm::GlobalValue* gv);

    // 4.2.4 — the init surface of an accumulating module: every
    // llvm.global_ctors entry not in `deliveredCtors`, its reference
    // closure, and a rebuilt llvm.global_ctors naming exactly those. The
    // names of the ctors taken are added to `deliveredCtors` on success.
    // Returns a FALSE (empty) ThreadSafeModule when nothing is new —
    // deliver nothing, run nothing. Everything the extract references but
    // does not define arrives later through the generator (spec 2.1); this
    // is what keeps a delivered module from binding every class's
    // vtable/RTTI/thunk chain at cell 1.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    extractInitDelta(llvm::Module* live,
                     std::set<std::string>& deliveredCtors);

} // namespace cajeta
