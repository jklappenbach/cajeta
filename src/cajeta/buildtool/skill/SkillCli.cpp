//
// CLI adapter helpers for skill discovery. See SkillCli.h and
// docs/specs/skill-discovery-spec.md §1.5.1.
//
#include "cajeta/buildtool/skill/SkillCli.h"

#include "cajeta/buildtool/skill/EmbeddedStdlibSkills.h"
#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/compile/CajetaArchive.h"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <exception>

namespace cajeta::buildtool::skill {

    namespace {

        // Match `--flag value` or `--flag=value`. On match, sets `out` and
        // advances `i` past any consumed value. Returns true if `args[i]` was
        // this flag.
        bool matchValueFlag(llvm::ArrayRef<std::string> args, size_t& i,
                            llvm::StringRef flag, std::optional<std::string>& out) {
            llvm::StringRef a = args[i];
            if (a == flag) {
                if (i + 1 < args.size()) {
                    out = args[i + 1];
                    ++i;
                }
                return true;
            }
            std::string eq = (flag + "=").str();
            if (a.starts_with(eq)) {
                out = a.drop_front(eq.size()).str();
                return true;
            }
            return false;
        }

    } // namespace

    SearchSkillArgs parseSearchSkillArgs(llvm::ArrayRef<std::string> args) {
        SearchSkillArgs r;
        bool haveName = false;
        for (size_t i = 1; i < args.size(); ++i) { // args[0] is the subcommand
            llvm::StringRef a = args[i];
            if (a == "--exact") {
                r.exact = true;
            } else if (matchValueFlag(args, i, "--version", r.version)) {
            } else if (matchValueFlag(args, i, "--from", r.from)) {
            } else if (a.starts_with("--")) {
                r.valid = false; // unknown flag
            } else if (!haveName) {
                r.name = a.str();
                haveName = true;
            } else {
                r.valid = false; // extra positional
            }
        }
        if (!haveName) r.valid = false;
        return r;
    }

    ListSkillsArgs parseListSkillsArgs(llvm::ArrayRef<std::string> args) {
        ListSkillsArgs r;
        bool haveScope = false;
        for (size_t i = 1; i < args.size(); ++i) {
            llvm::StringRef a = args[i];
            if (matchValueFlag(args, i, "--version", r.version)) {
            } else if (matchValueFlag(args, i, "--from", r.from)) {
            } else if (a.starts_with("--")) {
                r.valid = false;
            } else if (!haveScope) {
                r.scope = a.str();
                haveScope = true;
            } else {
                r.valid = false;
            }
        }
        return r;
    }

    std::vector<std::string> splitCommaUris(llvm::StringRef arg) {
        std::vector<std::string> out;
        llvm::SmallVector<llvm::StringRef, 8> parts;
        arg.split(parts, ',');
        for (llvm::StringRef p : parts) {
            llvm::StringRef t = p.trim();
            if (!t.empty()) out.push_back(t.str());
        }
        return out;
    }

    std::string formatSearchResults(llvm::ArrayRef<SkillSearchResult> results) {
        std::string out;
        llvm::raw_string_ostream os(out);
        for (const auto& r : results) os << r.uri << "\t" << r.matchedName << "\n";
        return os.str();
    }

    std::string formatListEntries(llvm::ArrayRef<SkillListEntry> entries) {
        std::string out;
        llvm::raw_string_ostream os(out);
        for (const auto& e : entries) os << e.uri << "\t" << e.title << "\n";
        return os.str();
    }

    std::string searchSkillUsage() {
        return "Usage: cajeta search-skill <name> [--version <v>] [--from <module>] "
               "[--exact]\n";
    }
    std::string listSkillsUsage() {
        return "Usage: cajeta list-skills [<scope>] [--version <v>] [--from <module>]\n";
    }
    std::string getSkillsUsage() {
        return "Usage: cajeta get-skills <uri>[,<uri>...]\n";
    }

    llvm::Expected<SkillSearchContext> loadSkillSearchContext(
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef)>
            lookupArtifact) {
        SkillSearchContext ctx;
        // Always-available stdlib skills (spec §2.5): seed the embedded stdlib
        // archives before any lockfile packages, so discovery returns stdlib
        // skills with no project / lockfile / dependencies present.
        const auto& embedded = embeddedStdlibSkillArchives();
        ctx.archives.insert(ctx.archives.end(), embedded.begin(), embedded.end());
        for (const ResolvedPackageEntry& p : packages) {
            // Consumer-scoping map: a workspace member sees its own resolutions.
            if (!p.memberOwner.empty()) {
                ctx.moduleVersions[p.memberOwner][p.name] = p.version;
            }

            std::optional<std::string> path = lookupArtifact(p.checksum);
            if (!path) continue; // not cached → nothing to read

            try {
                CajetaArchive arc = CajetaArchive::readFrom(*path);
                const CajetaArchiveEntry* idx = arc.findEntry("skills/index.json");
                if (!idx) continue; // package ships no skills
                std::string json(idx->data.begin(), idx->data.end());
                auto index = SkillIndex::deserialize(json, p.name + " index");
                if (!index) return index.takeError();
                ctx.archives.push_back({p.name, p.version, std::move(*index)});
            } catch (const std::exception& ex) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    ("reading '" + *path + "': " + ex.what()));
            }
        }
        return ctx;
    }

} // namespace cajeta::buildtool::skill
