//
// Skill Get core (skill-discovery spec §2.1). Resolves one or more
// `cja-skill://` URIs to their authored payloads by opening the resolved `.cja`
// archives offline (no network). Transport-agnostic: a CLI/MCP adapter wraps it.
//
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/Lockfile.h"

namespace cajeta::buildtool::skill {

    // The outcome of resolving one input URI. Exactly one of `payload`/`error`
    // is meaningful; `ok()` says which. Keyed back to the input `uri`.
    struct SkillGetResult {
        std::string uri;
        std::string payload; // authored skill bytes (frontmatter + body), verbatim
        std::string error;   // non-empty on failure
        bool ok() const { return error.empty(); }
    };

    // Resolve each URI to its authored payload. A bad/missing URI yields a
    // per-URI error without failing the others (one result per input, in order).
    //   `packages`       — resolved lockfile entries (for `<library>@<version>`).
    //   `lookupArtifact` — checksum → local `.cja` path (e.g. ArtifactCache).
    // Offline: reads only local archives.
    std::vector<SkillGetResult> getSkills(
        llvm::ArrayRef<std::string> uris,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
