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

    // JIT-merge only: promote every drop-thunk definition to weak_odr and shed its
    // comdat, so llvm::Linker cannot discard a linkonce_odr thunk nothing references yet
    // and leave a later consumer's extern dangling. The AOT path must NOT do this.
    void pinDropFunctionDefinitions(const std::vector<CajetaModulePtr>& modules);

} // namespace cajeta
