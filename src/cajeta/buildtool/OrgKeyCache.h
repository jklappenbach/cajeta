// Fetching and caching organization key documents — publisher-trust
// spec §6.1, §6.4, §2.5.
//
// The one place that turns "which repository, which organization" into a
// VERIFIED key document. Drivers hand back unverified bytes on purpose
// (they hold no trust anchors); this is where roots are resolved for the
// repository, the envelope is checked, and the answer is remembered.
//
// The cache is bounded by the document's own validity window rather than a
// TTL of our choosing. A document past its not-after is dropped and
// refetched, never served: serving a stale document is how
// revocation-by-expiry gets bypassed, and a cache is the most natural place
// for that to happen by accident.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/Repository.h"
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

        // The verified key document `repo` serves for `org`.
        //
        // Three outcomes, and they are deliberately distinct:
        //   * a document — verified, unexpired, usable;
        //   * nullopt — the repository serves none, which is spec 5.4's
        //     degrade condition;
        //   * an error — a fetch failed, or a document was served and did
        //     not verify. Neither is absence, and treating either as
        //     absence turns an outage or an attack into a bypass.
        llvm::Expected<std::optional<OrgKeyDocument>> documentFor(
            const Repository& repo, const std::string& org, std::time_t now);

        // How many times a document was actually fetched. Exists so a test
        // can tell a cache hit from a refetch — the only way to show that
        // an expired entry is not served.
        int fetches() const;

    private:
        RootTrustLayout layout_;
        mutable std::mutex mu_;
        std::map<std::string, OrgKeyDocument> cache_;
        int fetches_ = 0;
    };

} // namespace cajeta::buildtool
