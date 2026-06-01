#include "cajeta/buildtool/repo/TimingRepository.h"

#include <chrono>

namespace cajeta::buildtool {

    namespace {
        using Clock = std::chrono::steady_clock;
        ResolverTimings::Duration elapsed(Clock::time_point t0) {
            return std::chrono::duration_cast<ResolverTimings::Duration>(
                Clock::now() - t0);
        }
    } // namespace

    llvm::Expected<std::vector<std::string>>
    TimingRepository::listVersions(
        const std::string& depName) const {
        auto t0 = Clock::now();
        auto r = inner_->listVersions(depName);
        if (timings_) {
            timings_->listVersions += elapsed(t0);
            ++timings_->listVersionsCalls;
        }
        return r;
    }

    llvm::Expected<std::string> TimingRepository::fetch(
        const std::string& depName,
        const std::string& version) const {
        auto t0 = Clock::now();
        auto r = inner_->fetch(depName, version);
        if (timings_) {
            timings_->fetch += elapsed(t0);
            ++timings_->fetchCalls;
        }
        return r;
    }

    llvm::Expected<std::optional<std::string>>
    TimingRepository::fetchManifestJson(
        const std::string& depName,
        const std::string& version) const {
        auto t0 = Clock::now();
        auto r = inner_->fetchManifestJson(depName, version);
        if (timings_) {
            timings_->fetchManifest += elapsed(t0);
            ++timings_->fetchManifestCalls;
        }
        return r;
    }

    std::vector<RepositoryPtr> wrapWithTimings(
        const std::vector<RepositoryPtr>& repos,
        ResolverTimings* timings) {
        if (!timings) return repos;
        std::vector<RepositoryPtr> out;
        out.reserve(repos.size());
        for (const auto& r : repos) {
            out.push_back(std::make_shared<TimingRepository>(r, timings));
        }
        return out;
    }

} // namespace cajeta::buildtool
