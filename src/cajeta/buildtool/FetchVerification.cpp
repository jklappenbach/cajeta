#include "cajeta/buildtool/FetchVerification.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/KeyRevocation.h"
#include "cajeta/buildtool/PublisherVerification.h"
#include "cajeta/buildtool/ReleaseIntegrity.h"
#include "cajeta/buildtool/RootTrust.h"
#include "cajeta/buildtool/Signature.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include "cajeta/error/Diagnostics.h"

#include <filesystem>

namespace cajeta::buildtool {

    namespace {
        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // The publisher signature over the archive, when the repository
        // publishes one. With a signed owning organization its key document
        // decides; without one, the machine's trusted keys do (spec 9.1, 9.2).
        llvm::Error checkArchiveSignature(const Repository& repo,
                                          const std::string& name,
                                          const std::string& version,
                                          const std::string& artifactPath,
                                          const std::string& signature,
                                          const std::string& organization,
                                          const RepositoryDelegation* delegation,
                                          const cli::TrustStoreLayout& trust,
                                          OrgKeyCache& orgKeys,
                                          std::time_t now) {
            const std::string what = "'" + name + "' " + version + " from "
                                   + repo.name();
            if (!organization.empty()) {
                auto doc = orgKeys.documentFor(repo, organization, now);
                if (!doc) {
                    return err("the key document for '" + organization + "' from "
                               + repo.name() + " could not be used: "
                               + llvm::toString(doc.takeError()));
                }
                if (doc->has_value()) {
                    // Consulted BEFORE the signature and failing closed: failing
                    // open would un-revoke every key.
                    auto revocation = revocationFor(repo, delegation, now, 0);
                    if (!revocation) return revocation.takeError();
                    auto verdict = verifyAgainstOrgDocument(
                        **doc, name, artifactPath, signature, now,
                        revocation->has_value() ? &**revocation : nullptr);
                    // The document DECIDES: falling through to the trust store on
                    // a failure would be the bypass.
                    if (!verdict.ok()) return err(verdict.message);
                    return llvm::Error::success();
                }
            }
            std::vector<std::string> keys;
            for (const auto& entry : cli::listTrustedKeys(trust)) {
                keys.push_back(entry.path);
            }
            if (keys.empty()) {
                return err(what + " is signed, but this machine trusts no "
                           "signing keys, so the signature cannot be checked. "
                           "Add the publisher's key with `cajeta trust add`.");
            }
            auto verified = verifyAgainstAnyKey(artifactPath, signature, keys);
            if (!verified) {
                return err("the signature for " + what + " could not be checked: "
                           + llvm::toString(verified.takeError()));
            }
            if (!verified->has_value()) {
                return err("the signature for " + what + " does not match any "
                           "trusted key (" + std::to_string(keys.size())
                           + " checked). If this publisher is new, add their "
                             "key with `cajeta trust add`.");
            }
            return llvm::Error::success();
        }
    } // namespace

    namespace {
        // The release's integrity as the repository's roots and delegation
        // see it. Present metadata that does not verify is an error.
        struct Integrity {
            ReleaseIntegrity value;
            std::optional<RepositoryDelegation> delegation;
        };

        llvm::Expected<Integrity> integrityOf(const Repository& repo,
                                              const std::string& name,
                                              const std::string& version,
                                              const cli::TrustStoreLayout& trust,
                                              OrgKeyCache& orgKeys,
                                              std::time_t now) {
            auto roots = rootsFor(cli::rootTrustLayoutOf(trust), repo.name());
            if (!roots) {
                return err("the trust anchors for repository '" + repo.name()
                           + "' could not be resolved: "
                           + llvm::toString(roots.takeError()));
            }
            auto delegation = orgKeys.delegationFor(repo, now);
            if (!delegation) {
                return err("the delegation for repository '" + repo.name()
                           + "' could not be used: "
                           + llvm::toString(delegation.takeError()));
            }
            Integrity out;
            out.delegation = std::move(*delegation);
            auto integrity = releaseIntegrityFor(
                repo, name, version, *roots,
                out.delegation ? &*out.delegation : nullptr, now);
            if (!integrity) return integrity.takeError();
            out.value = std::move(*integrity);
            return out;
        }
    } // namespace

    llvm::Expected<std::string> expectedHashFor(const Repository& repo,
                                                const std::string& name,
                                                const std::string& version,
                                                const cli::TrustStoreLayout& trust,
                                                OrgKeyCache& orgKeys,
                                                std::time_t now) {
        auto i = integrityOf(repo, name, version, trust, orgKeys, now);
        if (!i) return i.takeError();
        return i->value.sha256;
    }

    llvm::Expected<VerifiedRelease> verifyFetchedRelease(
        const Repository& repo,
        const std::string& name,
        const std::string& version,
        const std::string& artifactPath,
        Verification policy,
        const cli::TrustStoreLayout& trust,
        OrgKeyCache& orgKeys,
        std::time_t now) {
        const std::string what = "'" + name + "' " + version + " from repository '"
                               + repo.name() + "'";
        // Present metadata that does not verify is an error here under either
        // policy; the relaxation only covers metadata that is ABSENT.
        auto checked = integrityOf(repo, name, version, trust, orgKeys, now);
        if (!checked) {
            return err("refusing " + what + ": "
                       + llvm::toString(checked.takeError()));
        }
        const ReleaseIntegrity* integrity = &checked->value;
        const RepositoryDelegation* deleg =
            checked->delegation ? &*checked->delegation : nullptr;
        if (!integrity->fromSignedMetadata && policy == Verification::Publisher) {
            return err("refusing " + what + ": the repository serves no signed "
                       "release metadata for it, so its publisher cannot be "
                       "verified. If you trust this repository's contents, relax "
                       "it in cajeta.json with \"verification\": \"checksum\" on "
                       "its settings.repositories entry (spec 5.4).");
        }

        VerifiedRelease out;
        out.sha256 = integrity->sha256;
        out.fromSignedMetadata = integrity->fromSignedMetadata;
        if (integrity->fromSignedMetadata) out.organization = integrity->organization;

        if (!integrity->sha256.empty()) {
            std::string actual = ArtifactCache::sha256OfFile(artifactPath);
            if (actual != integrity->sha256) {
                return err("refusing " + what + ": checksum mismatch. "
                           + (integrity->fromSignedMetadata
                                  ? "The root-signed release metadata says "
                                  : "The published checksum says ")
                           + integrity->sha256 + ", the bytes hash to "
                           + (actual.empty() ? std::string("nothing") : actual)
                           + ". The download was discarded.");
            }
        }

        auto sig = repo.publishedSignature(name, version);
        if (!sig) return sig.takeError();
        if (sig->has_value()) {
            if (auto e = checkArchiveSignature(repo, name, version, artifactPath,
                                               **sig, out.organization, deleg,
                                               trust, orgKeys, now)) {
                return err("refusing " + what + ": " + llvm::toString(std::move(e)));
            }
        }

        // Retraction warns and does not refuse: a lockfile pinning this version
        // must keep resolving.
        if (integrity->retracted) {
            cajeta::logLine("warn",
                what + " is RETRACTED by its publisher: "
                + (integrity->retractedReason.empty()
                       ? std::string("no reason given")
                       : integrity->retractedReason)
                + (integrity->fromSignedMetadata
                       ? ""
                       : " (unsigned metadata, so treat it as advisory)")
                + "\n");
        }

        if (auto env = repo.releaseMetadataJson(name, version)) {
            if (env->has_value()) out.envelopeJson = **env;
        } else {
            llvm::consumeError(env.takeError());
        }
        return out;
    }

    VerifyingRepository::VerifyingRepository(RepositoryPtr inner,
                                             Verification policy,
                                             cli::TrustStoreLayout trust,
                                             std::shared_ptr<OrgKeyCache> orgKeys)
        : inner_(std::move(inner)), policy_(policy), trust_(std::move(trust)),
          orgKeys_(std::move(orgKeys)) {}

    llvm::Expected<std::string> VerifyingRepository::fetch(
        const std::string& depName, const std::string& version) const {
        auto path = inner_->fetch(depName, version);
        if (!path) return path.takeError();
        auto verified = verifyFetchedRelease(*inner_, depName, version, *path,
                                             policy_, trust_, *orgKeys_,
                                             std::time(nullptr));
        if (!verified) {
            // A filesystem repository hands back its own file; anything else
            // staged a download, which a refusal discards (spec 5.2).
            if (!dynamic_cast<const FilesystemRepository*>(inner_.get())) {
                std::error_code ec;
                std::filesystem::remove(*path, ec);
            }
            return verified.takeError();
        }
        if (!verified->envelopeJson.empty()) {
            std::lock_guard<std::mutex> lock(mu_);
            verifiedEnvelopes_[depName + "@" + version] = verified->envelopeJson;
        }
        return path;
    }

    llvm::Expected<std::optional<std::string>>
    VerifyingRepository::releaseMetadataJson(const std::string& n,
                                             const std::string& v) const {
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = verifiedEnvelopes_.find(n + "@" + v);
            if (it != verifiedEnvelopes_.end()) {
                return std::optional<std::string>{it->second};
            }
        }
        return inner_->releaseMetadataJson(n, v);
    }

    std::vector<RepositoryPtr> wrapWithVerification(
        const std::vector<RepositoryPtr>& repos,
        const std::vector<RepositorySpec>& specs,
        const cli::TrustStoreLayout& trust) {
        auto orgKeys = std::make_shared<OrgKeyCache>(cli::rootTrustLayoutOf(trust));
        std::vector<RepositoryPtr> out;
        out.reserve(repos.size());
        for (size_t i = 0; i < repos.size(); ++i) {
            Verification policy = i < specs.size() ? specs[i].verification
                                                   : Verification::Publisher;
            out.push_back(std::make_shared<VerifyingRepository>(
                repos[i], policy, trust, orgKeys));
        }
        return out;
    }

} // namespace cajeta::buildtool
