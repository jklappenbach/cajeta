// The verifying cache behind OrgKeyCache.h.

#include "cajeta/buildtool/OrgKeyCache.h"

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // `\x1f` is in neither name, so no two pairs can collide into one entry.
        std::string cacheKey(const std::string& repo, const std::string& org) {
            return repo + "\x1f" + org;
        }

    } // namespace

    int OrgKeyCache::fetches() const {
        std::lock_guard<std::mutex> lk(mu_);
        return fetches_;
    }

    llvm::Expected<std::optional<OrgKeyDocument>> OrgKeyCache::documentFor(
            const Repository& repo, const std::string& org, std::time_t now) {
        const std::string key = cacheKey(repo.name(), org);
        std::time_t seenIssuedAt = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto seen = seenIssuedAt_.find(key);
            if (seen != seenIssuedAt_.end()) seenIssuedAt = seen->second;
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                if (now < it->second.notAfter) {
                    return std::optional<OrgKeyDocument>{it->second};
                }
                // An expired document is refused however it was obtained.
                cache_.erase(it);
            }
        }

        auto bytes = repo.organizationKeys(org);
        if (!bytes) return bytes.takeError();
        if (!bytes->has_value()) {
            // Absence, not failure: the caller decides what that permits.
            return std::optional<OrgKeyDocument>{};
        }

        auto roots = rootsFor(layout_, repo.name());
        if (!roots) return roots.takeError();
        if (roots->empty()) {
            return err("repository '" + repo.name() + "' serves a key "
                       "document for '" + org + "', but this machine trusts "
                       "no root key for that repository, so it cannot be "
                       "checked");
        }

        // Enforced HERE: a refetch is when a mirror can hand back an older document.
        auto doc = loadOrgKeyDocument(**bytes, *roots, now, seenIssuedAt);
        if (!doc) return doc.takeError();
        if (doc->organization != org) {
            // Or a repository could answer every request with one org's document.
            return err("repository '" + repo.name() + "' served a key "
                       "document for '" + doc->organization + "' when asked "
                       "for '" + org + "'");
        }

        std::lock_guard<std::mutex> lk(mu_);
        ++fetches_;
        cache_[key] = *doc;
        // The mark outlives the document: expiry must not reopen the replay.
        auto& high = seenIssuedAt_[key];
        if (doc->issuedAt > high) high = doc->issuedAt;
        return std::optional<OrgKeyDocument>{*doc};
    }

    llvm::Expected<std::optional<RepositoryDelegation>>
    OrgKeyCache::delegationFor(const Repository& repo, std::time_t now) {
        const std::string key = repo.name();
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = delegations_.find(key);
            if (it != delegations_.end()) {
                if (now < it->second.notAfter) {
                    return std::optional<RepositoryDelegation>{it->second};
                }
                delegations_.erase(it);
            }
        }

        auto bytes = repo.repositoryKeys();
        if (!bytes) return bytes.takeError();
        if (!bytes->has_value()) {
            return std::optional<RepositoryDelegation>{};
        }

        auto roots = rootsFor(layout_, repo.name());
        if (!roots) return roots.takeError();
        if (roots->empty()) {
            return err("repository '" + repo.name() + "' serves a delegation, "
                       "but this machine trusts no root key for it, so it "
                       "cannot be checked");
        }

        // The origin binding is enforced inside loadRepositoryDelegation against
        // repo.origin(); comparing repo.name() here too refuses real deployments.
        std::time_t seenIssuedAt = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto seen = seenIssuedAt_.find(key);
            if (seen != seenIssuedAt_.end()) seenIssuedAt = seen->second;
        }

        auto del = loadRepositoryDelegation(**bytes, *roots, repo.origin(), now,
                                            seenIssuedAt);
        if (!del) return del.takeError();

        std::lock_guard<std::mutex> lk(mu_);
        ++fetches_;
        auto& high = seenIssuedAt_[key];
        if (del->issuedAt > high) high = del->issuedAt;
        delegations_[key] = *del;
        return std::optional<RepositoryDelegation>{*del};
    }

} // namespace cajeta::buildtool
