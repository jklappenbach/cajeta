// Retry with exponential backoff for transient network failures, bounded in
// both backoff and attempts so a permanent failure surfaces in seconds.

#pragma once

#include <llvm/Support/Error.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace cajeta::buildtool {

    struct RetryPolicy {
        int maxAttempts = 3;   // 1 = one attempt, no retries
        std::chrono::milliseconds initialBackoff{200};
        std::chrono::milliseconds maxBackoff{4000};   // each backoff doubles to here
        // Transient (retry) or permanent (surface now)? The classifier sees the
        // STRINGIFIED error, which is where the actions tag their failures.
        std::function<bool(const std::string&)> isTransient;
    };

    // Run `attempt`, retrying while the policy calls the error transient and
    // attempts remain. The last error propagates out once they exhaust.
    template <typename Fn>
    auto retryWithBackoff(const RetryPolicy& policy, Fn&& attempt)
        -> decltype(attempt()) {
        using ResultT = decltype(attempt());
        auto backoff = policy.initialBackoff;
        for (int i = 0; i < policy.maxAttempts; ++i) {
            ResultT r = attempt();
            if (r) return r;
            std::string msg = llvm::toString(r.takeError());
            bool transient = !policy.isTransient ||
                             policy.isTransient(msg);
            if (i + 1 >= policy.maxAttempts || !transient) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(), msg);
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(policy.maxBackoff,
                               backoff + backoff);
        }
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
            "retryWithBackoff: maxAttempts must be >= 1");
    }

    // 408 / 425 / 429 / 5xx → transient.
    bool isTransientHttpStatus(long status);

    // Connect/resolve/send/recv-timeout → transient. CURLE_OK never arrives here.
    bool isTransientCurlCode(int curlCode);

    // The default classifier: parses `[curl=N]` and `(status=N)` out of a message.
    bool defaultNetworkTransient(const std::string& msg);

} // namespace cajeta::buildtool
