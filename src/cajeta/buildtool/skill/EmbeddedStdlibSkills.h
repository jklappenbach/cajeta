// Always-available stdlib skills (skill-discovery spec §2.5): the stdlib is embedded
// as source, so its skills ride along as a zstd corpus exposed here as archives the
// discovery context is always seeded with — no project, lockfile or deps needed.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillSearch.h"

namespace cajeta::buildtool::skill {

    // The library version stamped on every embedded stdlib skill archive and URI.
    extern const char* const kStdlibSkillVersion;

    // One ResolvedSkillArchive per stdlib library, built once and cached; empty when the corpus is absent.
    const std::vector<ResolvedSkillArchive>& embeddedStdlibSkillArchives();

    std::optional<std::string> embeddedStdlibSkillPayload(
        llvm::StringRef library, llvm::StringRef id);

} // namespace cajeta::buildtool::skill
