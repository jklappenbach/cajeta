// Shared drop-function backfill (jit-drop-backfill spec §2). Drop thunks are
// synthesized lazily into the owning type's module, so a consumer whose synthesis
// never fired is left with a bare extern; this pass finds those and fills them in.
#pragma once

#include <string>
#include <vector>

#include "cajeta/compile/CajetaModule.h"

namespace cajeta {

    // The exact symbol getOrCreate{Stack,}DropFunction emits for a canonical type name:
    // `__cajeta[_stack]_<mangled>_drop`, mangling ':' '.' '<' '>' ',' ' ' to '_'.
    std::string dropSymbolName(const std::string& canonicalTypeName, bool stack);

    // Scan `modulesToScan` for undefined `__cajeta[_stack]_<type>_drop` declarations and
    // synthesize the owning class's wrappers. Only a class whose emit module is in
    // `currentModules` is backfilled; an earlier compile's modules are already freed.
    void backfillDropFunctions(const std::vector<CajetaModulePtr>& modulesToScan,
                               const std::vector<CajetaModulePtr>& currentModules);

    // Promote every drop-thunk definition to weak_odr, so nothing can discard a
    // linkonce_odr thunk that its own module never calls and leave another module's
    // extern dangling. The JIT merge (`shedComdat`) also drops the comdat, because
    // llvm::Linker discards lazily at the in-process merge. Object emission keeps the
    // comdat for the system linker to merge, and pins only because each module is
    // optimized alone, where GlobalDCE deletes an uncalled linkonce_odr thunk.
    void pinDropFunctionDefinitions(const std::vector<CajetaModulePtr>& modules,
                                    bool shedComdat = true);

} // namespace cajeta
