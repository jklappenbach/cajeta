#include "cajeta/buildtool/Retry.h"

#include <curl/curl.h>

namespace cajeta::buildtool {

    bool isTransientHttpStatus(long status) {
        // 408 Request Timeout — load-balancer hand-off retries.
        // 425 Too Early — caller may retry.
        // 429 Too Many Requests — backoff exactly the right answer.
        // 500-504 server-side hiccups.
        if (status == 408 || status == 425 || status == 429) return true;
        if (status >= 500 && status < 600) return true;
        return false;
    }

    bool isTransientCurlCode(int curlCode) {
        switch (curlCode) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_PARTIAL_FILE:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_HTTP2_STREAM:
            return true;
        default:
            return false;
        }
    }

    bool defaultNetworkTransient(const std::string& msg) {
        auto parse = [&](const std::string& prefix) -> long {
            auto p = msg.find(prefix);
            if (p == std::string::npos) return -1;
            auto end = msg.find_first_of("])", p);
            if (end == std::string::npos) return -1;
            try {
                return std::stol(msg.substr(p + prefix.size(),
                                            end - p - prefix.size()));
            } catch (...) { return -1; }
        };
        long curlCode = parse("[curl=");
        if (curlCode >= 0) return isTransientCurlCode(
            static_cast<int>(curlCode));
        long status = parse("(status=");
        if (status >= 0) return isTransientHttpStatus(status);
        return false;
    }

} // namespace cajeta::buildtool
