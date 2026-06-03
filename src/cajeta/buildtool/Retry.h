// Retry with exponential backoff for transient network failures.
//
// Cajeta-side network actions (upload, publish, repo fetch) wrap
// their attempt in retryWithBackoff so a single 503 from a CDN or a
// dropped TCP connection doesn't fail the build. Backoff doubles
// per attempt (capped) and the maximum attempt count is bounded so
// a permanent failure surfaces in seconds, not minutes.

#pragma once

#include <llvm/Support/Error.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace cajeta::buildtool {

    struct RetryPolicy {
        // 1 = no retries (one attempt). 3 attempts means try once,
        // then up to two retries.
        int maxAttempts = 3;
        std::chrono::milliseconds initialBackoff{200};
        // Each successive backoff doubles up to this cap.
        std::chrono::milliseconds maxBackoff{4000};
        // Classify the error message as transient (retry) vs
        // permanent (surface immediately). The classifier sees the
        // stringified error, NOT the typed llvm::Error — this matches
        // how Phase 9 actions tag failures (`[curl=N]`, `(status=N)`
        // markers embedded in the message).
        std::function<bool(const std::string&)> isTransient;
    };

    // Run `attempt`. On success: return its result. On error: if the
    // policy says transient AND attempts remain, sleep the current
    // backoff and try again. The last error propagates out when all
    // retries exhaust.
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

    // Connect/resolve/send/recv-timeout → transient. CURLE_OK never
    // appears in this path (not an error).
    bool isTransientCurlCode(int curlCode);

    // Default classifier the upload + publish actions install on
    // their policy: parses `[curl=N]` and `(status=N)` tags out of
    // the message string.
    bool defaultNetworkTransient(const std::string& msg);

} // namespace cajeta::buildtool
