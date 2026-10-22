// Build-time skill packaging (skill-discovery spec §4.2): discovers `<root>/skills/*.md`,
// validates each, indexes them, and produces the members to embed in the `.cja`.
#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace cajeta {
    class CajetaArchive;
}

namespace cajeta::buildtool::skill {

    struct SkillMember {
        std::string path;  // "skills/<id>.md" or "skills/index.json"
        std::string bytes;
    };

    // The members to embed — each `skills/<id>.md` verbatim plus `skills/index.json`,
    // path-sorted. No skills is an empty list; an invalid or duplicate id is an error.
    llvm::Expected<std::vector<SkillMember>> buildSkillMembers(llvm::StringRef packageRoot);

    // Builds the members and adds them to `archive` as Origin::User resources.
    llvm::Error addSkillMembersToArchive(CajetaArchive& archive,
                                         llvm::StringRef packageRoot);

    // Exception-throwing variant; throws std::runtime_error on a packaging failure.
    void addSkillMembersToArchiveOrThrow(CajetaArchive& archive,
                                         llvm::StringRef packageRoot);

    // Skills the packager will not pack, one message each: a `skills/` holding
    // `.md` files anywhere under `sourceRoot` other than `<skillRoot>/skills`,
    // and a project's `skills/` beside the nearest cajeta.json above
    // `sourceRoot` when `skillRoot` is somewhere else (the bare-compiler
    // form, whose skill root is the source root). Empty when nothing is missed.
    std::vector<std::string> misplacedSkillWarnings(llvm::StringRef skillRoot,
                                                    llvm::StringRef sourceRoot);

} // namespace cajeta::buildtool::skill
