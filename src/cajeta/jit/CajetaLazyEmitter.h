#pragma once

// Generate one method's body and package it for ORC. The front-end keeps owning
// its llvm::Module, so ORC only ever receives a bitcode SNAPSHOT reparsed into
// its own context. Call with the CompilerGate held: codegen touches global state.

#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"

#include <set>
#include <string>

namespace llvm { class GlobalValue; class Module; }

namespace cajeta {

    // A snapshot defining `method`'s body alone, everything else a declaration.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    emitMethodModule(const MethodPtr& method);

    // A definition synthesized outside the method table, snapshotted as-is.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    snapshotLiveDefinition(llvm::GlobalValue* gv);

    // The init surface an accumulating module has gained: every llvm.global_ctors
    // entry not in `deliveredCtors` plus its reference closure, added to that set
    // on success. A FALSE (empty) module means nothing is new — run nothing.
    llvm::Expected<llvm::orc::ThreadSafeModule>
    extractInitDelta(llvm::Module* live,
                     std::set<std::string>& deliveredCtors);

    // Mark every ctor `live` defines as delivered, for a WHOLE-module delivery.
    void recordDeliveredCtors(llvm::Module* live,
                              std::set<std::string>& deliveredCtors);

} // namespace cajeta
