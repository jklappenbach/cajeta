// The `cja-skill://<library>@<version>/<skill-id>` URI scheme and its lockfile
// resolver. The version is the RESOLVED one, so a held URI is a stable cache key.
#pragma once

#include <optional>
#include <string>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/Lockfile.h"

namespace cajeta::buildtool::skill {

    struct SkillUri {
        std::string library;  // dependency coordinate, e.g. "cajeta.io"
        std::string version;  // resolved version, e.g. "1.4.2"
        std::string skillId;  // skill id within the library, e.g. "file-open"

        // Rejects a wrong scheme, a missing `@` or version, an empty part.
        static llvm::Expected<SkillUri> parse(llvm::StringRef text);

        // Canonical `cja-skill://…` form; round-trips parse().
        std::string format() const;

        bool operator==(const SkillUri& o) const {
            return library == o.library && version == o.version &&
                   skillId == o.skillId;
        }
    };

    // Resolve `<library>@<version>` to a local `.cja` path. `packages` is matched
    // by EXACT name + version, never a range; `lookupArtifact` maps a checksum to
    // a cache path. Errors when the coordinate or the artifact is missing.
    llvm::Expected<std::string> resolveSkillArchive(
        const SkillUri& uri,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
