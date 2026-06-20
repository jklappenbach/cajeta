//
// Build-time skill packaging (skill-discovery spec §4.2). Discovers a package's
// hand-authored skills under `<root>/skills/*.md` (the chosen convention — clean
// git diffs, §4.2), validates each (D.1), builds the per-package index (D.2), and
// produces the in-archive members to embed in the `.cja`.
//
#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace cajeta {
    class CajetaArchive;
}

namespace cajeta::buildtool::skill {

    // One member to embed in the `.cja`: an in-archive path and its bytes.
    struct SkillMember {
        std::string path;  // "skills/<id>.md" or "skills/index.json"
        std::string bytes;
    };

    // Discover + validate + index a package's skills, returning the members to
    // embed (each `skills/<id>.md` verbatim plus `skills/index.json`), sorted by
    // path for reproducibility (spec §4.2 / D.3.3). No `skills/` dir, or one with
    // no `*.md`, yields an empty list (no index member) — a package without
    // skills is not an error. An invalid skill or a duplicate id is an error.
    llvm::Expected<std::vector<SkillMember>> buildSkillMembers(llvm::StringRef packageRoot);

    // Convenience: build the members and add them to `archive` as Resource
    // entries (Origin::User). A no-op when the package has no skills.
    llvm::Error addSkillMembersToArchive(CajetaArchive& archive,
                                         llvm::StringRef packageRoot);

    // Exception-throwing variant for call sites that propagate errors via
    // exceptions (e.g. the compiler's archive emitter). Throws std::runtime_error
    // on a packaging failure.
    void addSkillMembersToArchiveOrThrow(CajetaArchive& archive,
                                         llvm::StringRef packageRoot);

} // namespace cajeta::buildtool::skill
