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

namespace llvm { class GlobalValue; }

namespace cajeta {

    llvm::Expected<llvm::orc::ThreadSafeModule>
    emitMethodModule(const MethodPtr& method);

    // A definition codegen synthesized outside the method table — a drop
    // thunk, a vtable or #ClassObject global — snapshotted as-is; no
    // generateCode, it already exists in a live module.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    snapshotLiveDefinition(llvm::GlobalValue* gv);

} // namespace cajeta
