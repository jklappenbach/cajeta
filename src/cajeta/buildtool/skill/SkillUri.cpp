//
// cja-skill:// URI parse/format + lockfile resolver. See SkillUri.h and
// specs/archive/skill-discovery-spec.md §2.2.
//
#include "cajeta/buildtool/skill/SkillUri.h"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Error.h>

namespace cajeta::buildtool::skill {

    namespace {

        constexpr llvm::StringLiteral kScheme = "cja-skill://";

        llvm::Error uriError(const llvm::Twine& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           ("skill URI: " + msg).str());
        }

    } // namespace

    llvm::Expected<SkillUri> SkillUri::parse(llvm::StringRef text) {
        if (!text.starts_with(kScheme)) {
            return uriError("expected scheme '" + llvm::Twine(kScheme) + "' in '" +
                            text + "'");
        }
        llvm::StringRef rest = text.drop_front(kScheme.size());

        // <library>@<version>/<skill-id>
        auto [coord, skillId] = rest.split('/');
        if (skillId.empty()) {
            return uriError("missing skill id in '" + text + "'");
        }
        auto [library, version] = coord.split('@');
        if (library.empty()) {
            return uriError("missing library in '" + text + "'");
        }
        // split('@') with no '@' yields empty version.
        if (!coord.contains('@') || version.empty()) {
            return uriError("missing version (expected '<library>@<version>') in '" +
                            text + "'");
        }

        SkillUri uri;
        uri.library = library.str();
        uri.version = version.str();
        uri.skillId = skillId.str();
        return uri;
    }

    std::string SkillUri::format() const {
        return (kScheme + library + "@" + version + "/" + skillId).str();
    }

    llvm::Expected<std::string> resolveSkillArchive(
        const SkillUri& uri,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef)>
            lookupArtifact) {
        const ResolvedPackageEntry* match = nullptr;
        for (const auto& p : packages) {
            // Exact name + version: the URI pins a resolved version, never a range.
            if (p.name == uri.library && p.version == uri.version) {
                match = &p;
                break;
            }
        }
        if (!match) {
            return uriError("'" + uri.library + "@" + uri.version +
                            "' is not in the lockfile");
        }
        if (std::optional<std::string> path = lookupArtifact(match->checksum)) {
            return *path;
        }
        return uriError("'" + uri.library + "@" + uri.version +
                        "' is resolved but its archive is not in the local cache "
                        "(checksum " + match->checksum + ")");
    }

} // namespace cajeta::buildtool::skill
