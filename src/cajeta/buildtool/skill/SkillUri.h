//
// `cja-skill://` URI scheme + lockfile resolver (skill-discovery spec §2.2).
// A skill URI is a logical, resolvable identity:
//
//   cja-skill://<library>@<version>/<skill-id>
//   e.g.  cja-skill://cajeta.io@1.4.2/file-open
//
// The <version> is the *resolved* library version, so a held URI is a stable
// cache key and Get is always exact (spec §3.4).
//
#pragma once

#include <optional>
#include <string>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/Lockfile.h"

namespace cajeta::buildtool::skill {

    // A parsed skill URI.
    struct SkillUri {
        std::string library;  // dependency coordinate, e.g. "cajeta.io"
        std::string version;  // resolved version, e.g. "1.4.2"
        std::string skillId;  // skill id within the library, e.g. "file-open"

        // Parse a `cja-skill://<library>@<version>/<skill-id>` URI. Rejects a
        // wrong scheme, a missing `@`/version, an empty library, or an empty id.
        static llvm::Expected<SkillUri> parse(llvm::StringRef text);

        // Render back to canonical `cja-skill://…` form (round-trips parse()).
        std::string format() const;

        bool operator==(const SkillUri& o) const {
            return library == o.library && version == o.version &&
                   skillId == o.skillId;
        }
    };

    // Resolve a skill URI's `<library>@<version>` to a local `.cja` path.
    //   `packages`        — the resolved lockfile entries (matched by exact
    //                       name + version, so a returned URI's version pins the
    //                       resolved archive, never a range — spec §3.4 / D.4.3).
    //   `lookupArtifact`  — maps an entry's checksum to a local cache path,
    //                       typically `ArtifactCache::lookup`; nullopt on a cache
    //                       miss.
    // Errors: the coordinate isn't in the lockfile, or it is but the artifact
    // isn't in the local cache.
    llvm::Expected<std::string> resolveSkillArchive(
        const SkillUri& uri,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
