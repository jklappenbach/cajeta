#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {
    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;
}

namespace cajeta {
namespace xpu {

    /** What the launch sites say about each @Kernel's block. */
    struct LaunchBlocks {
        std::unordered_map<std::string, unsigned> maxThreads;      // simple name -> largest constant block
        std::unordered_map<std::string, unsigned> unboundedSites;  // simple name -> non-constant sites
        std::unordered_map<std::string, std::array<unsigned, 3>> pinned;  // qualified name -> the one block
    };

    /** Scans every launch site in `modules` and sets or clears each kernel's pinned launch block. */
    LaunchBlocks scanLaunchBlocks(const std::vector<CajetaModulePtr>& modules);

} // namespace xpu
} // namespace cajeta
