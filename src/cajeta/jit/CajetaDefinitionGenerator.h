#pragma once

// Resolve a missing symbol by generating the body the eager loop would have
// emitted. `resolve()` holds the decision and is plain testable code, with
// `tryToGenerate` a thin ORC adapter; cross-generator entry is CompilerGate's job.

#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cajeta {

    // One thread inside the compiler at a time, PROCESS-WIDE (ORC serializes only
    // per generator). Recursive, since emitting a body can re-enter a lookup.
    class CompilerGate {
    public:
        static CompilerGate& instance();

        void run(const std::function<void()>& fn);

        static bool heldByThisThread();

        size_t maxThreadsObserved() const { return maxInside.load(); }
        void resetObservation() { maxInside.store(0); }

        void runUngatedForTest(const std::function<void()>& fn);

    private:
        void observe(const std::function<void()>& fn);

        std::recursive_mutex mutex;
        std::atomic<size_t> inside{0};
        std::atomic<size_t> maxInside{0};
    };

    class CajetaDefinitionGenerator : public llvm::orc::DefinitionGenerator {
    public:
        // Host-supplied: how a finished snapshot reaches the JIT. What to emit stays
        // the generator's job.
        using DeliverFn = std::function<llvm::Error(llvm::orc::ThreadSafeModule,
                                                    llvm::orc::JITDylib&)>;

        explicit CajetaDefinitionGenerator(CajetaSymbolIndex& index,
                                           DeliverFn deliver = nullptr)
            : index(index), deliver(std::move(deliver)) {}

        // The methods this generator will emit for `symbols`; empty when lazy emission
        // is off. An unknown symbol is not ours: a fall-through, not an error.
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
        DeliverFn deliver;
        // All guarded by the CompilerGate — only one thread emits at a time.
        size_t generated = 0;
        long long emitNs = 0;
        // One delivery defines both __emutls_v./_t.; the sibling must not repeat it.
        std::set<std::string> servedEmutls;
    };

} // namespace cajeta
