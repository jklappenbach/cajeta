#include "cajeta/jit/LazyCodegen.h"

#include <atomic>
#include <cstdlib>
#include <string>

namespace cajeta {

    namespace {
        // ELF only: the lazy delivery path does not yet speak Mach-O or COFF
        // symbol conventions, so those hosts default to EAGER.
        // `CAJETA_LAZY_CODEGEN=1` still opts in anywhere.
#if defined(__APPLE__) || defined(_WIN32)
        constexpr bool kDefaultLazy = false;
#else
        constexpr bool kDefaultLazy = true;
#endif

        // Atomic because ORC reads it from materialization threads while a host
        // configures; relaxed, since it orders no other memory.
        std::atomic<bool>& modeCell() {
            // Seeded ONCE from the environment, then owned by the setter, so no
            // hot path pays for a getenv and the value stays writable.
            static std::atomic<bool> cell{[] {
                const char* eager = std::getenv("CAJETA_EAGER_CODEGEN");
                if (eager && std::string(eager) == "1") return false;
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
