// Skill Get core (skill-discovery spec §2.1): resolves `cja-skill://` URIs to their
// authored payloads by opening the resolved `.cja` archives offline.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/Lockfile.h"

namespace cajeta::buildtool::skill {

    struct SkillGetResult {
        std::string uri;
        std::string payload; // authored skill bytes (frontmatter + body), verbatim
        std::string error;   // non-empty on failure
        bool ok() const { return error.empty(); }
    };

    // Resolves each URI to its payload, one result per input in order, so a bad URI fails
    // only itself. `packages` are lockfile entries, `lookupArtifact` maps checksum → path.
    std::vector<SkillGetResult> getSkills(
        llvm::ArrayRef<std::string> uris,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
