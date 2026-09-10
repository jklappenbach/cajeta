// Transport-agnostic CLI glue for skill discovery: args, formatting, context.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/skill/SkillGet.h"
#include "cajeta/buildtool/skill/SkillSearch.h"
#include "cajeta/dap/Json.h"

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

    // Parse args after the subcommand name. Flags: --version, --from, --exact
    // (search only); the first bare arg is the name / scope.
    SearchSkillArgs parseSearchSkillArgs(llvm::ArrayRef<std::string> args);
    ListSkillsArgs parseListSkillsArgs(llvm::ArrayRef<std::string> args);

    std::vector<std::string> splitCommaUris(llvm::StringRef arg);

    // --- output formatting ---

    std::string formatSearchResults(llvm::ArrayRef<SkillSearchResult> results);
    std::string formatListEntries(llvm::ArrayRef<SkillListEntry> entries);

    // The `--json` shapes, shared verbatim with the compiler-mcp tools: search →
    // [{uri, matchedName, tier, distance}], list → [{uri, title, names}].
    cajeta::dap::Json searchResultsJsonValue(
        llvm::ArrayRef<SkillSearchResult> results);
    cajeta::dap::Json listEntriesJsonValue(llvm::ArrayRef<SkillListEntry> entries);
    cajeta::dap::Json getResultsJsonValue(llvm::ArrayRef<SkillGetResult> results);

    std::string searchSkillUsage();
    std::string listSkillsUsage();
    std::string getSkillsUsage();

    // --- context assembly ---

    // Build a context from resolved lockfile packages. An uncached or index-less
    // package is skipped, but a corrupt `skills/index.json` is an error.
    llvm::Expected<SkillSearchContext> loadSkillSearchContext(
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef checksum)>
            lookupArtifact);

} // namespace cajeta::buildtool::skill
