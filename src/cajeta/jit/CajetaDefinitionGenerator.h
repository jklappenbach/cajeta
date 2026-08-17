#pragma once

// lazy-codegen 2.2.1 (spec 3.1, 3.2) — resolve a missing symbol by generating
// the body the eager loop would have emitted.
//
// Split deliberately in two: `resolve()` is the decision (which of these
// symbols are ours?) and is plain, testable code; `tryToGenerate` is a thin ORC
// adapter over it. The decision is where the bugs live — claiming a symbol we
// cannot emit turns a clean fall-through into a failed materialization.
//
// ORC serializes entry per generator (Core.h: a mutex, an InUse flag, and a
// PendingLookups deque), so a second concurrent lookup parks rather than
// re-entering the compiler's single-threaded global state.

#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/Core.h"

#include <functional>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaDefinitionGenerator : public llvm::orc::DefinitionGenerator {
    public:
        // Generate `method`'s body and deliver the module that now defines it.
        // Host-supplied: the kernel and the JIT host add modules differently.
        using EmitFn = std::function<llvm::Error(const MethodPtr&,
                                                 llvm::orc::JITDylib&)>;

        explicit CajetaDefinitionGenerator(CajetaSymbolIndex& index,
                                           EmitFn emit = nullptr)
            : index(index), emit(std::move(emit)) {}

        // The methods this generator will emit for `symbols`. Empty when lazy
        // emission is off, so Unit 2 is inert until Unit 4 flips the default.
        // A symbol we do not know is silently not ours — that is an ordinary
        // fall-through, not an error.
        std::vector<MethodPtr> resolve(
            const std::vector<std::string>& symbols) const;

        llvm::Error tryToGenerate(llvm::orc::LookupState& state,
                                  llvm::orc::LookupKind kind,
                                  llvm::orc::JITDylib& jd,
                                  llvm::orc::JITDylibLookupFlags flags,
                                  const llvm::orc::SymbolLookupSet& lookupSet)
            override;

        size_t generatedCount() const { return generated; }

    private:
        CajetaSymbolIndex& index;
        EmitFn emit;
        size_t generated = 0;   // guarded by ORC's per-generator serialization
    };

} // namespace cajeta
