#include "cajeta/jit/LazyCodegen.h"

#include <atomic>
#include <cstdlib>
#include <string>

namespace cajeta {

    namespace {
        // Unit 2 lands the generator BEHIND the eager path, so the default is
        // eager and `CAJETA_EAGER_CODEGEN=1` only pins what is already true.
        // Unit 4 flips this one line; the env var becomes load-bearing then,
        // and stays a permanent control (spec 5.1).
        constexpr bool kDefaultLazy = false;

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
