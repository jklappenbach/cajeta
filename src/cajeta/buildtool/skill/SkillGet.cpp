//
// Skill Get core. See SkillGet.h and
// specs/archive/skill-discovery-spec.md §2.1.
//
#include "cajeta/buildtool/skill/SkillGet.h"

#include "cajeta/buildtool/skill/EmbeddedStdlibSkills.h"
#include "cajeta/buildtool/skill/SkillUri.h"
#include "cajeta/compile/CajetaArchive.h"

#include <llvm/Support/Error.h>

#include <exception>

namespace cajeta::buildtool::skill {

    namespace {

        // Resolve one URI to its payload, or set `out.error`.
        void getOne(llvm::StringRef uriText,
                    llvm::ArrayRef<ResolvedPackageEntry> packages,
                    llvm::function_ref<std::optional<std::string>(llvm::StringRef)>
                        lookupArtifact,
                    SkillGetResult& out) {
            auto uri = SkillUri::parse(uriText);
            if (!uri) {
                out.error = llvm::toString(uri.takeError());
                return;
            }
            // Always-available stdlib skills (spec §2.5): resolve embedded stdlib
            // payloads before any lockfile archive lookup.
            if (uri->version == kStdlibSkillVersion) {
                if (auto payload =
                        embeddedStdlibSkillPayload(uri->library, uri->skillId)) {
                    out.payload = std::move(*payload);
                    return;
                }
            }
            auto archivePath = resolveSkillArchive(*uri, packages, lookupArtifact);
            if (!archivePath) {
                out.error = llvm::toString(archivePath.takeError());
                return;
            }

            std::string member = "skills/" + uri->skillId + ".md";
            try {
                CajetaArchive arc = CajetaArchive::readFrom(*archivePath);
                const CajetaArchiveEntry* e = arc.findEntry(member);
                if (!e) {
                    out.error = "skill '" + uri->skillId + "' not found in archive '" +
                                *archivePath + "'";
                    return;
                }
                out.payload.assign(e->data.begin(), e->data.end());
            } catch (const std::exception& ex) {
                out.error = "reading '" + *archivePath + "': " + ex.what();
            }
        }

    } // namespace

    std::vector<SkillGetResult> getSkills(
        llvm::ArrayRef<std::string> uris,
        llvm::ArrayRef<ResolvedPackageEntry> packages,
        llvm::function_ref<std::optional<std::string>(llvm::StringRef)>
            lookupArtifact) {
        std::vector<SkillGetResult> out;
        out.reserve(uris.size());
        for (llvm::StringRef u : uris) {
            SkillGetResult r;
            r.uri = u.str();
            getOne(u, packages, lookupArtifact, r);
            out.push_back(std::move(r));
        }
        return out;
    }

} // namespace cajeta::buildtool::skill
