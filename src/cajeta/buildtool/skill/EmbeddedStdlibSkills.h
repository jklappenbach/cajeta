//
// Always-available stdlib skills (skill-discovery spec §2.5). The stdlib is
// embedded in the compiler as source, not a resolved `.cja`, so its skills are
// embedded as a zstd-compressed corpus (tools/skillembed) and exposed here as
// ResolvedSkillArchives the discovery context is always seeded with — so
// search/list/get return stdlib skills with no project, lockfile, or deps.
//
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillSearch.h"

namespace cajeta::buildtool::skill {

    // The library version stamped on every embedded stdlib skill archive/URI
    // (`cja-skill://cajeta.<pkg>@<ver>/<id>`). Matches `cajeta-lang-version`.
    extern const char* const kStdlibSkillVersion;

    // One ResolvedSkillArchive per stdlib library (`cajeta.<pkg>`), built once
    // (decompress + parse) and cached. Empty if the embedded corpus is absent.
    const std::vector<ResolvedSkillArchive>& embeddedStdlibSkillArchives();

    // The decompressed payload of an embedded stdlib skill, or nullopt if the
    // (library, id) pair is not embedded.
    std::optional<std::string> embeddedStdlibSkillPayload(
        llvm::StringRef library, llvm::StringRef id);

} // namespace cajeta::buildtool::skill
