// TimingRepository — Repository decorator that records each
// underlying call's wall-clock duration + a call count into a
// `ResolverTimings` instance. Installed by
// `resolveProjectDependencies` when timings are requested
// (`cajeta info --resolve-time`).
//
// Wraps any existing RepositoryPtr; forwards every call to the
// inner instance. The wrapper holds a non-owning pointer to the
// caller's `ResolverTimings`, so the timing instance must outlive
// the wrapper.

#pragma once

#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"

#include <memory>
#include <string>

namespace cajeta::buildtool {

    class TimingRepository : public Repository {
    public:
        TimingRepository(RepositoryPtr inner, ResolverTimings* timings)
            : inner_(std::move(inner)), timings_(timings) {}

        std::string name() const override { return inner_->name(); }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& depName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& depName,
            const std::string& version) const override;

        llvm::Expected<std::optional<std::string>> fetchManifestJson(
            const std::string& depName,
            const std::string& version) const override;

    private:
        RepositoryPtr inner_;
        ResolverTimings* timings_;
    };

    // Wrap each repo in a TimingRepository pinned to `timings`. The
    // resulting vector mirrors the input one-to-one; pass through
    // when `timings` is nullptr.
    std::vector<RepositoryPtr> wrapWithTimings(
        const std::vector<RepositoryPtr>& repos,
        ResolverTimings* timings);

} // namespace cajeta::buildtool
