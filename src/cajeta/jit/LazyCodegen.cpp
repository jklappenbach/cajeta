#include "cajeta/jit/LazyCodegen.h"

#include <atomic>
#include <cstdlib>
#include <string>

namespace cajeta {

    namespace {
        // Flipped by Unit 4 (4.2.2, 2026-08-17): bodies are emitted on
        // demand by default. `CAJETA_EAGER_CODEGEN=1` restores the eager
        // fixpoint and stays a permanent supported control (spec 5.1) —
        // keep both paths exercised (Unit 7).
        //
        // ELF hosts only, for now. The lazy delivery path does not yet
        // speak Mach-O or COFF symbol conventions: the v0.21.0 dry-run
        // (2026-08-18) had every kernel-cell JIT fail to materialize on
        // both — darwin missing underscore-mangled globals and the
        // __emutls_v/__lazyp runtime stubs, mingw missing the printf
        // shims (__mingw_fprintf) from lazy modules — while the same
        // suites were green on aarch64-linux. Until the delivery layer
        // learns those formats those hosts default to EAGER;
        // CAJETA_LAZY_CODEGEN=1 still opts in anywhere.
#if defined(__APPLE__) || defined(_WIN32)
        constexpr bool kDefaultLazy = false;
#else
        constexpr bool kDefaultLazy = true;
#endif

        // Atomic because ORC calls the generator from materialization threads
        // while a host may still be configuring. Relaxed is enough: this gates
        // which of two correct paths runs, and never orders other memory.
        std::atomic<bool>& modeCell() {
            // Seeded ONCE from the environment, then owned by the setter. The
            // seed is a static initialiser rather than a per-read getenv so a
            // hot path does not pay for it; the value stays writable, which is
            // the whole point of 2.1.5.
            static std::atomic<bool> cell{[] {
                // Explicit eager wins — it is the permanent escape hatch.
                const char* eager = std::getenv("CAJETA_EAGER_CODEGEN");
                if (eager && std::string(eager) == "1") return false;
                // Force-enable for A/B from outside the process (battery runs,
                // the DynCol repro) before Unit 4 flips the default.
                const char* lazy = std::getenv("CAJETA_LAZY_CODEGEN");
                if (lazy && std::string(lazy) == "1") return true;
                return kDefaultLazy;
            }()};
            return cell;
        }
    } // namespace

    bool lazyCodegenEnabled() {
        return modeCell().load(std::memory_order_relaxed);
    }

    void setLazyCodegenEnabled(bool enabled) {
        modeCell().store(enabled, std::memory_order_relaxed);
    }

} // namespace cajeta
