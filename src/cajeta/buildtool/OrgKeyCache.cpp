#include "cajeta/buildtool/OrgKeyCache.h"

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // A repository name and an org name both reach this key. `\x1f`
        // cannot appear in either, so no pair of them can collide into one
        // entry — which would let one repository answer for another.
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
                // Past its window. Drop it and go back to the repository:
                // an expired document must be refused however it was
                // obtained, and "from our own cache" is still obtained.
                cache_.erase(it);
            }
        }

        auto bytes = repo.organizationKeys(org);
        if (!bytes) return bytes.takeError();
        if (!bytes->has_value()) {
            // Absence, not failure. The caller decides what a repository
            // that vouches for nobody is allowed to install (spec 5.4).
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

        // The freshness rule is applied HERE and not merely available in
        // the parser. An expired entry is dropped above and refetched, and
        // a refetch is precisely when a mirror gets to hand back an older
        // document (spec 2.9).
        auto doc = loadOrgKeyDocument(**bytes, *roots, now, seenIssuedAt);
        if (!doc) return doc.takeError();
        if (doc->organization != org) {
            // The document has to speak for the org we asked about.
            // Without this, a repository could answer every request with
            // one organization's document and borrow its namespaces.
            return err("repository '" + repo.name() + "' served a key "
                       "document for '" + doc->organization + "' when asked "
                       "for '" + org + "'");
        }

        std::lock_guard<std::mutex> lk(mu_);
        ++fetches_;
        cache_[key] = *doc;
        // Remember the high-water mark even after the document itself is
        // evicted for expiry, or every expiry would reopen the replay.
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

        auto del = loadRepositoryDelegation(**bytes, *roots, repo.origin(), now);
        if (!del) return del.takeError();
        if (del->repository != repo.name()) {
            // The delegation has to speak for the repository we asked. Without
            // this, one repository's delegation could be replayed by another
            // and its online key would sign for both.
            return err("repository '" + repo.name() + "' served a delegation "
                       "for '" + del->repository + "'");
        }

        std::lock_guard<std::mutex> lk(mu_);
        ++fetches_;
        delegations_[key] = *del;
        return std::optional<RepositoryDelegation>{*del};
    }

} // namespace cajeta::buildtool
