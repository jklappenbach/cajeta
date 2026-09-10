// Clone-and-extract repository driver: one instance is one git source pinned to
// one ref, cloned lazily under `<stageDir>/git/<hash(url,ref)>/` by the `git`
// binary. `fetch` expects a pre-built `.cja` already in `<subdir>/build/archive/`.

#pragma once

#include "cajeta/buildtool/Repository.h"

#include <mutex>
#include <string>

namespace cajeta::buildtool {

    class GitRepository : public Repository {
    public:
        GitRepository(std::string name,
                      std::string cloneUrl,
                      std::string gitRef,
                      std::string gitSubdir,
                      std::string stageDir);
        ~GitRepository() override;

        GitRepository(const GitRepository&) = delete;
        GitRepository& operator=(const GitRepository&) = delete;

        std::string name() const override { return name_; }
        // The clone URL, which is this repository's stable identity.
        std::string origin() const override { return cloneUrl_; }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& packageName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& packageName,
            const std::string& version) const override;

        llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& packageName,
            const std::string& version) const override;

    private:
        // Clone into the deterministic stage path and check out the configured
        // ref. Idempotent, and cheap to re-call once the clone exists.
        llvm::Error ensureClone() const;

        // Read the checked-out cajeta.json and cache the `(packageName, version)`
        // it declares, on the first successful read.
        llvm::Error ensureMetadata() const;

        // The directory holding the dep's cajeta.json: clone root plus any subdir.
        std::string checkoutDir() const;

        // Truncated SHA-256(url + "\n" + ref), the clone dir name under git/.
        static std::string hashKey(const std::string& url,
                                   const std::string& ref);

        std::string name_;
        std::string cloneUrl_;
        std::string gitRef_;
        std::string gitSubdir_;
        std::string stageDir_;
        std::string cloneDir_;  // <stageDir>/git/<key>

        // Lazily populated.
        mutable std::mutex mu_;
        mutable bool cloned_ = false;
        mutable bool metadataLoaded_ = false;
        mutable std::string declaredName_;
        mutable std::string declaredVersion_;
        mutable std::string manifestJsonBytes_;
    };

} // namespace cajeta::buildtool
