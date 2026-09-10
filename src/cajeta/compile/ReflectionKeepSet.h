#pragma once

// DCE Tier-0b keep-set resolution (lean-linker-dce.md §3.2), shared by the Lean-mode
// link and the lazy kernel so "what must survive reflection" has ONE answer.

#include <map>
#include <memory>
#include <set>
#include <string>

namespace cajeta {

    // Resolves the accumulated reflection sites into the class canonical names whose
    // registration ctor must be kept; NULL keeps everything, `keptBy` gets the reason.
    std::shared_ptr<const std::set<std::string>>
    resolveReflectionKeepSet(std::map<std::string, std::string>* keptBy = nullptr);

} // namespace cajeta
