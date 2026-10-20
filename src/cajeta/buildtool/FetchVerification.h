// What a fetched release is held to before anything trusts its bytes
// (publisher-trust spec 4.1-4.2, 5.2-5.5; plan Unit 6).
//
// Each repository a manifest declares carries a `verification` policy.
// "publisher", the default, requires release metadata signed through the
// repository's root or delegation, and the bytes must hash to the value it
// names. "checksum" is the 5.4 relaxation: an unsigned release is admitted on
// the repository's own checksum. The relaxation is per repository (5.5), and
// metadata that is PRESENT and does not verify is refused under either policy:
// that is tampering, not absence.
//
// The built-in ~/.olla store is not a declared repository and is never
// wrapped. Nothing enters it unverified except the machine's own
// `cajeta install` builds.

#pragma once

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/cli/TrustStore.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // What a release passed. `envelopeJson` is the release metadata exactly as
    // the repository served it, empty when it served none.
    struct VerifiedRelease {
        std::string sha256;
        bool fromSignedMetadata = false;
        std::string organization;
        std::string envelopeJson;
    };

    // A release olla published before it signed release metadata
    // (2026-09-03), pinned by the exact bytes it served.
    struct LegacyRelease {
        std::string name;
        std::string version;
        std::string sha256;
    };

    // The legacy releases this toolchain vouches for, measured against live olla
    // 2026-10-20: each archive downloaded and hashed. The set is closed, since
    // olla signs every publish now.
    const std::vector<LegacyRelease>& legacyReleases();

    // The one check every fetch path runs on `artifactPath`, the bytes `repo`
    // just produced for `name@version`. An error means refuse and keep nothing.
    // Under the default policy a release with no signed metadata is admitted
    // only when `legacy` (the embedded list when null) names it with these
    // exact bytes.
    llvm::Expected<VerifiedRelease> verifyFetchedRelease(
        const Repository& repo,
        const std::string& name,
        const std::string& version,
        const std::string& artifactPath,
        Verification policy,
        const cli::TrustStoreLayout& trust,
        OrgKeyCache& orgKeys,
        std::time_t now,
        const std::vector<LegacyRelease>* legacy = nullptr);

    // The hash `name@version` from `repo` is published under, signed or not.
    // It finds a cached copy; it decides nothing, so a hit still goes through
    // verifyFetchedRelease.
    llvm::Expected<std::string> expectedHashFor(const Repository& repo,
                                                const std::string& name,
                                                const std::string& version,
                                                const cli::TrustStoreLayout& trust,
                                                OrgKeyCache& orgKeys,
                                                std::time_t now);

    // A declared repository whose `fetch` returns bytes only after they pass
    // its policy. A refused download that the repository staged is deleted.
    class VerifyingRepository : public Repository {
    public:
        VerifyingRepository(RepositoryPtr inner, Verification policy,
                            cli::TrustStoreLayout trust,
                            std::shared_ptr<OrgKeyCache> orgKeys);

        std::string name() const override { return inner_->name(); }
        std::string origin() const override { return inner_->origin(); }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& depName) const override {
            return inner_->listVersions(depName);
        }

        llvm::Expected<std::string> fetch(
            const std::string& depName,
            const std::string& version) const override;

        llvm::Expected<std::optional<std::string>> fetchManifestJson(
            const std::string& depName,
            const std::string& version) const override {
            return inner_->fetchManifestJson(depName, version);
        }

        llvm::Expected<std::optional<std::string>> publishedChecksum(
            const std::string& n, const std::string& v) const override {
            return inner_->publishedChecksum(n, v);
        }
        llvm::Expected<std::optional<std::string>> publishedSignature(
            const std::string& n, const std::string& v) const override {
            return inner_->publishedSignature(n, v);
        }
        llvm::Expected<std::optional<std::string>> organizationKeys(
            const std::string& org) const override {
            return inner_->organizationKeys(org);
        }
        llvm::Expected<std::optional<std::string>> repositoryKeys() const override {
            return inner_->repositoryKeys();
        }
        llvm::Expected<RepoCapabilities> capabilities() const override {
            return inner_->capabilities();
        }
        llvm::Expected<std::optional<std::string>> revocations() const override {
            return inner_->revocations();
        }

        // The envelope the last verified fetch of `name@version` was checked
        // against, so the write-through stores what was checked without a
        // second round trip; otherwise the repository's own answer.
        llvm::Expected<std::optional<std::string>> releaseMetadataJson(
            const std::string& n, const std::string& v) const override;

    private:
        RepositoryPtr inner_;
        Verification policy_;
        cli::TrustStoreLayout trust_;
        std::shared_ptr<OrgKeyCache> orgKeys_;
        mutable std::mutex mu_;
        mutable std::map<std::string, std::string> verifiedEnvelopes_;
    };

    // Wraps each of `repos` in a VerifyingRepository with the policy of the spec
    // it was built from: `repos` is buildRepositories(`specs`), one-to-one.
    std::vector<RepositoryPtr> wrapWithVerification(
        const std::vector<RepositoryPtr>& repos,
        const std::vector<RepositorySpec>& specs,
        const cli::TrustStoreLayout& trust);

} // namespace cajeta::buildtool
