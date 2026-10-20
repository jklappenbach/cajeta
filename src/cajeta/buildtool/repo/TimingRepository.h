// TimingRepository — a Repository decorator recording each underlying call's
// duration and count into a caller-owned ResolverTimings, which must outlive it.

#pragma once

#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"

#include <string>

namespace cajeta::buildtool {

    class TimingRepository : public Repository {
    public:
        TimingRepository(RepositoryPtr inner, ResolverTimings* timings)
            : inner_(std::move(inner)), timings_(timings) {}

        std::string name() const override { return inner_->name(); }
        // A decorator borrows its subject's identity; inventing one would make the same repository verify differently when timed.
        std::string origin() const override { return inner_->origin(); }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& depName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& depName,
            const std::string& version) const override;

        llvm::Expected<std::optional<std::string>> fetchManifestJson(
            const std::string& depName,
            const std::string& version) const override;

        // The trust surface is forwarded untimed: a decorator that answered
        // "nothing published" here would make a timed run verify differently.
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
        llvm::Expected<std::optional<std::string>> releaseMetadataJson(
            const std::string& n, const std::string& v) const override {
            return inner_->releaseMetadataJson(n, v);
        }

    private:
        RepositoryPtr inner_;
        ResolverTimings* timings_;
    };

    // Wraps each repo in a TimingRepository pinned to `timings`; the result mirrors
    // the input one-to-one, and passes through unchanged when `timings` is null.
    std::vector<RepositoryPtr> wrapWithTimings(
        const std::vector<RepositoryPtr>& repos,
        ResolverTimings* timings);

} // namespace cajeta::buildtool
