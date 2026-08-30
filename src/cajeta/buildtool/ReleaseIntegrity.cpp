#include "cajeta/buildtool/ReleaseIntegrity.h"

#include "cajeta/buildtool/ReleaseMetadata.h"

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

    } // namespace

    llvm::Expected<ReleaseIntegrity> releaseIntegrityFor(
            const Repository& repo,
            const std::string& name,
            const std::string& version,
            const std::vector<RootKey>& roots) {
        ReleaseIntegrity out;

        auto raw = repo.releaseMetadataJson(name, version);
        if (!raw) {
            // A failure to ASK is not an answer. Fall through to the
            // sidecar rather than reporting a hash we did not get, and
            // let the fetch produce the real diagnostic if the repository
            // is genuinely broken.
            llvm::consumeError(raw.takeError());
        } else if (raw->has_value()) {
            auto md = loadReleaseMetadata(**raw, roots);
            if (!md) {
                return err("the release metadata for '" + name + "' "
                           + version + " from " + repo.name()
                           + " did not verify: "
                           + llvm::toString(md.takeError()));
            }
            if (md->signedByRoot && !md->sha256.empty()) {
                out.sha256 = md->sha256;
                out.fromSignedMetadata = true;
                out.organization = md->organization;
                out.rootKeyId = md->rootKeyId;
                return out;
            }
            // Present but unsigned. It carries no more authority than the
            // sidecar, so it gets no more weight: fall through rather
            // than treating a parsed hash as a verified one.
        }

        auto sidecar = repo.publishedChecksum(name, version);
        if (!sidecar) {
            llvm::consumeError(sidecar.takeError());
            return out;
        }
        if (sidecar->has_value()) out.sha256 = **sidecar;
        return out;
    }

} // namespace cajeta::buildtool
