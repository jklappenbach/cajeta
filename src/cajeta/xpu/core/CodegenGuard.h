#pragma once

#include <functional>
#include <string>

namespace cajeta {
namespace xpu {

    // Runs `emit` with an LLVM fatal error turned into a false return and its reason, so
    // one kernel that the backend cannot select is skipped by name instead of ending the build.
    bool runGuardedCodegen(const std::function<void()>& emit, std::string& reason);

}  // namespace xpu
}  // namespace cajeta
