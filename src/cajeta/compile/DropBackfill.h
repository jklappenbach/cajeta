//
// Shared drop-function backfill (jit-drop-backfill spec §2).
//
// Drop thunks — `__cajeta_stack_<type>_drop` / `__cajeta_<type>_drop` — are
// synthesized lazily into the owning type's module when a consumer's codegen
// drops a value of the type. A consumer whose synthesis never fired is left
// with a bare extern declaration: cached .bc from a skipped incremental
// module, or (JIT) instantiations created only indirectly during stdlib
// codegen, which the obligation set can't enumerate. This pass scans modules
// for undefined drop declarations and synthesizes exactly those, in the
// owning type's own module — the full-build layout.
//
// Both the AOT incremental path (Compiler) and the JIT pipeline
// (CajetaJitHost::buildJit) call this; the drop-symbol mangling lives here
// and nowhere else.
//
#pragma once

#include <string>
#include <vector>

#include "cajeta/compile/CajetaModule.h"

namespace cajeta {

    // The exact symbol getOrCreate{Stack,}DropFunction emits for a type's
    // canonical name: `__cajeta[_stack]_<mangled>_drop`, where mangling
    // replaces ':' '.' '<' '>' ',' ' ' with '_'.
    std::string dropSymbolName(const std::string& canonicalTypeName, bool stack);

    // Scan `modulesToScan` for undefined `__cajeta[_stack]_<type>_drop`
    // declarations and synthesize the owning class's wrappers (both families,
    // matching the historical AOT behavior). Callers choose the scan set: the
    // AOT incremental path passes its cache-loaded (incremental-clean)
    // modules; the JIT passes every compiled module.
    //
    // `currentModules` is the FULL module list of the compile in progress. It
    // gates synthesis: the canonical type map is a persistent thread-local,
    // so in a multi-compile process (JIT test harnesses, debug sessions) it
    // still holds classes from EARLIER compiles whose LLVM modules were
    // consumed by that compile's merge — synthesizing into those is a write
    // into freed memory. A matched class is backfilled only when its emit
    // module belongs to the current compile (pointer membership; stale
    // pointers are never dereferenced).
    void backfillDropFunctions(const std::vector<CajetaModulePtr>& modulesToScan,
                               const std::vector<CajetaModulePtr>& currentModules);

    // JIT-merge only. llvm::Linker lazy-links linkonce_odr definitions: a
    // drop thunk defined in a donor module that links while nothing in the
    // merged module references it yet is DISCARDED, and a later consumer's
    // extern declaration then dangles ("Symbols not found" at LLJIT
    // materialization — samples/tour, jit-drop-backfill spec §3). Promote
    // every drop-thunk definition to weak_odr (kept unconditionally, still
    // ODR-mergeable) and shed its comdat, so the definition survives merge
    // regardless of link order. The AOT path must NOT do this: its modules
    // reach the system linker as separate objects whose comdat sections
    // resolve correctly.
    void pinDropFunctionDefinitions(const std::vector<CajetaModulePtr>& modules);

} // namespace cajeta
