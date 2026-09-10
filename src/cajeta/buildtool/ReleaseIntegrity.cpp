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
            const std::vector<RootKey>& roots,
            const RepositoryDelegation* delegation,
            std::time_t now) {
        ReleaseIntegrity out;

        // Who may have signed the release metadata; a delegation narrows it to the online release keys.
        std::vector<RootKey> verifiers;
        if (delegation) {
            for (const auto* k : delegation->usableKeys(now)) {
                verifiers.push_back(RootKey{k->id, k->publicKeyPem, false});
            }
            if (verifiers.empty()) {
                return err("repository '" + repo.name() + "' delegates release "
                           "signing, but no delegated key is inside its "
                           "validity window right now, so nothing it serves "
                           "can be verified");
            }
            out.viaDelegation = true;
        } else {
            verifiers = roots;
        }

        auto raw = repo.releaseMetadataJson(name, version);
        if (!raw) {
            // A failure to ASK is not an answer — fall through to the sidecar rather than report a hash we never got.
            llvm::consumeError(raw.takeError());
        } else if (raw->has_value()) {
            auto md = loadReleaseMetadata(**raw, verifiers);
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
                // From the signed payload; the plain `retracted` beside it is ignored whenever an envelope is present (spec 7.6.2).
                out.retracted = md->retracted;
                out.retractedReason = md->retractedReason;
                return out;
            }
            // Present but unsigned: no more authority than the sidecar, so it does
            // not count as verified. The retraction still travels, with
            // `fromSignedMetadata` false, so it warns rather than binds.
            out.retracted = md->retracted;
            out.retractedReason = md->retractedReason;
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
