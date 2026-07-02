// GitRepository — clone-and-extract repository driver (Phase 6c).
//
// One instance represents one git source pinned to one ref (tag,
// branch, or commit hash). The dep it serves is determined by
// reading `details.name` + `details.version` from the cajeta.json
// at the checked-out tree's `<subdir>/cajeta.json`.
//
// Layout per BuildTool.md "Git repository":
//
//   <stageDir>/git/<hash(url,ref)>/        clone root
//   <stageDir>/git/<hash(url,ref)>/<subdir>/cajeta.json
//   <stageDir>/git/<hash(url,ref)>/<subdir>/build/archive/<n>-<v>.cja
//
// Clones happen lazily on first call. `git` is invoked as a child
// process; the driver fails clearly if the binary isn't on PATH.
//
// v1 limitation: `fetch` expects a pre-built `.cja` inside the
// checkout (under `<subdir>/build/archive/`). Spawning a recursive
// `cajeta build` against the clone is a future enhancement —
// today users vendoring via git are expected to ship the artifact
// alongside source, or run `cajeta build` in the clone manually.

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
        // Clone (or refresh) into the deterministic stage path and
        // check out the configured ref. Idempotent — re-callable
        // cheaply once the clone exists.
        llvm::Error ensureClone() const;

        // Read the checked-out cajeta.json and cache the
        // `(packageName, version)` it declares. Both fields cached
        // on first successful read.
        llvm::Error ensureMetadata() const;

        // Filesystem path to the directory containing the dep's
        // cajeta.json (clone root + optional subdir).
        std::string checkoutDir() const;

        // SHA-256(url + "\n" + ref) truncated, used as the clone dir
        // name under stageDir/git/.
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
