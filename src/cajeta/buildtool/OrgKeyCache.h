// The one place that turns "which repository, which organization" into a VERIFIED
// key document: drivers hold no trust anchors, so roots are resolved and envelopes
// checked here. Entries expire by the document's own window, never by a TTL.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/RepositoryDelegation.h"
#include "cajeta/buildtool/RootTrust.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace cajeta::buildtool {

    class OrgKeyCache {
    public:
        explicit OrgKeyCache(RootTrustLayout layout)
            : layout_(std::move(layout)) {}

        // The verified key document `repo` serves for `org`. Three distinct
        // outcomes: a usable document; nullopt when the repository serves none; an
        // error when a fetch failed or a served document did not verify.
        llvm::Expected<std::optional<OrgKeyDocument>> documentFor(
            const Repository& repo, const std::string& org, std::time_t now);

        // The verified delegation naming which keys may sign `repo`'s release
        // metadata. Same three outcomes as `documentFor`.
        llvm::Expected<std::optional<RepositoryDelegation>> delegationFor(
            const Repository& repo, std::time_t now);

        // How many times a document was actually fetched, so a test can tell a
        // cache hit from a refetch.
        int fetches() const;

    private:
        RootTrustLayout layout_;
        mutable std::mutex mu_;
        std::map<std::string, OrgKeyDocument> cache_;
        // Newest `issued-at` accepted per org. Outlives the cached document on
        // purpose: eviction on expiry must not reopen the replay window.
        std::map<std::string, std::time_t> seenIssuedAt_;
        std::map<std::string, RepositoryDelegation> delegations_;
        int fetches_ = 0;
    };

} // namespace cajeta::buildtool
