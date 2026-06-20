//
// CLI adapter helpers for skill discovery (skill-discovery spec §1.5.1). Pure,
// transport-agnostic glue: argument parsing, output formatting, and assembling a
// SkillSearchContext from resolved archives. The build-tool subcommands
// (search-skill / list-skills / get-skills) are thin wrappers over these + the
// Search/List/Get cores, so the same logic is reachable by a future MCP adapter.
//
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/skill/SkillSearch.h"

namespace cajeta::buildtool::skill {

    // --- argument parsing ---

    struct SearchSkillArgs {
        std::string name;
        std::optional<std::string> version;
        std::optional<std::string> from;
        bool exact = false;
        bool valid = true; // false → print usage
    };

    struct ListSkillsArgs {
        std::optional<std::string> scope;
        std::optional<std::string> version;
        std::optional<std::string> from;
        bool valid = true;
    };

    // Parse args after the subcommand name (argv[0] is the subcommand). Flags:
    // --version <v>/=<v>, --from <m>/=<m>, --exact (search only). First bare arg
    // is the name (search, required) / scope (list, optional).
    SearchSkillArgs parseSearchSkillArgs(llvm::ArrayRef<std::string> args);
    ListSkillsArgs parseListSkillsArgs(llvm::ArrayRef<std::string> args);

    // Split a comma-delimited URI list (the get-skills CLI form), trimming
    // whitespace and dropping empties.
    std::vector<std::string> splitCommaUris(llvm::StringRef arg);

    // --- output formatting ---

    // One line per result: "<uri>\t<matchedName>".
    std::string formatSearchResults(llvm::ArrayRef<SkillSearchResult> results);
    // One line per entry: "<uri>\t<title>".
    std::string formatListEntries(llvm::ArrayRef<SkillListEntry> entries);

    // Usage strings.
    std::string searchSkillUsage();
    std::string listSkillsUsage();
    std::string getSkillsUsage();

    // --- context assembly ---

    // Build a SkillSearchContext from resolved lockfile packages: for each
    // package, resolve its `.cja` via `lookupArtifact`, read `skills/index.json`,
    // and add a ResolvedSkillArchive. Packages not cached, or with no skill
    // index, are skipped; a corrupt index is an error. `moduleVersions` is built
    // from workspace `memberOwner` groupings so `--from <member>` resolves the
    // version that member sees (the settled module-identifier form, D.5.2).
    llvm::Expected<SkillSearchContext> loadSkillSearchContext(
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
