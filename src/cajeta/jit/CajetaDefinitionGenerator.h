#pragma once

// lazy-codegen 2.2.1 (spec 3.1, 3.2) — resolve a missing symbol by generating
// the body the eager loop would have emitted.
//
// Split deliberately in two: `resolve()` is the decision (which of these
// symbols are ours?) and is plain, testable code; `tryToGenerate` is a thin ORC
// adapter over it. The decision is where the bugs live — claiming a symbol we
// cannot emit turns a clean fall-through into a failed materialization.
//
// ORC serializes entry per generator object only (Core.h: a mutex, an InUse
// flag, a PendingLookups deque). Cross-generator serialization — several
// generators, one compiler world — is the CompilerGate below.

#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/Core.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cajeta {

    // 2.2.2 — one thread inside the compiler at a time, PROCESS-WIDE. ORC
    // serializes tryToGenerate per generator object; a process holds several
    // generators but one compiler world (canonicalMap, active module,
    // substitution stack). Recursive, because emitting a body can re-enter a
    // lookup on the same thread — still single-threaded, and a plain mutex
    // would deadlock the session at the first cascade (spec 3.4).
    class CompilerGate {
    public:
        static CompilerGate& instance();

        void run(const std::function<void()>& fn);

        // True while the calling thread is inside run().
        static bool heldByThisThread();

        // Observation, so tests ASSERT the discipline instead of trusting the
        // mutex: the most threads ever seen inside at once.
        size_t maxThreadsObserved() const { return maxInside.load(); }
        void resetObservation() { maxInside.store(0); }

        // Counts without locking — the control that proves the observation can
        // see concurrency, so maxThreadsObserved()==1 is never vacuous.
        void runUngatedForTest(const std::function<void()>& fn);

    private:
        void observe(const std::function<void()>& fn);

        std::recursive_mutex mutex;
        std::atomic<size_t> inside{0};
        std::atomic<size_t> maxInside{0};
    };

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
        // Both guarded by the CompilerGate — only one thread emits at a time.
        size_t generated = 0;
        long long emitNs = 0;
    };

} // namespace cajeta
