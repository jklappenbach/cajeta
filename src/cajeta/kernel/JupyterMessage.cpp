#include "cajeta/kernel/JupyterMessage.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace cajeta::kernel {

    namespace {

        const char* kDelimiter = "<IDS|MSG>";

        std::string toHex(const unsigned char* bytes, size_t len) {
            static const char* digits = "0123456789abcdef";
            std::string out;
            out.reserve(len * 2);
            for (size_t i = 0; i < len; ++i) {
                out.push_back(digits[bytes[i] >> 4]);
                out.push_back(digits[bytes[i] & 0x0f]);
            }
            return out;
        }

        // One RNG for the process. Seeded from the system source; a kernel
        // whose msg_ids collide would have a frontend correlating replies to
        // the wrong request.
        std::mt19937_64& rng() {
            static std::mt19937_64 engine([] {
                std::random_device rd;
                return (static_cast<uint64_t>(rd()) << 32) ^ rd();
            }());
            return engine;
        }

    }  // namespace

    const char* channelName(Channel c) {
        switch (c) {
            case Channel::Shell:   return "shell";
            case Channel::IOPub:   return "iopub";
            case Channel::Control: return "control";
            case Channel::Stdin:   return "stdin";
        }
        return "unknown";
    }

    MessageSigner::MessageSigner(std::string key, std::string scheme)
        : key_(std::move(key)), scheme_(std::move(scheme)) {}

    std::string MessageSigner::sign(const std::string& header,
                                    const std::string& parentHeader,
                                    const std::string& metadata,
                                    const std::string& content) const {
        if (!enabled()) return std::string();
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLen = 0;
        HMAC_CTX* ctx = HMAC_CTX_new();
        if (!ctx) return std::string();
        // The scheme field is `hmac-sha256` in every connection file Jupyter
        // writes; anything else is unsupported rather than silently treated
        // as sha256, so a mismatch fails closed at verify time.
        if (HMAC_Init_ex(ctx, key_.data(), static_cast<int>(key_.size()),
                         EVP_sha256(), nullptr) == 1) {
            HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(header.data()),
                        header.size());
            HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(parentHeader.data()),
                        parentHeader.size());
            HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(metadata.data()),
                        metadata.size());
            HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(content.data()),
                        content.size());
            HMAC_Final(ctx, digest, &digestLen);
        }
        HMAC_CTX_free(ctx);
        return toHex(digest, digestLen);
    }

    bool MessageSigner::verify(const std::string& signature,
                               const std::string& header,
                               const std::string& parentHeader,
                               const std::string& metadata,
                               const std::string& content) const {
        if (!enabled()) return true;
        if (scheme_ != "hmac-sha256") return false;
        std::string expected = sign(header, parentHeader, metadata, content);
        if (expected.empty() || expected.size() != signature.size()) return false;
        // Constant-time: a byte-at-a-time compare leaks the length of the
        // matching prefix, which is enough to forge a signature one byte at a
        // time against a kernel that will answer as fast as you can ask.
        return CRYPTO_memcmp(expected.data(), signature.data(),
                             expected.size()) == 0;
    }

    std::vector<std::string> encodeMessage(const JupyterMessage& msg,
                                           const MessageSigner& signer) {
        std::string header = msg.header.dump();
        std::string parent = msg.parentHeader.dump();
        std::string metadata = msg.metadata.dump();
        std::string content = msg.content.dump();

        std::vector<std::string> frames;
        frames.reserve(msg.identities.size() + 6 + msg.buffers.size());
        for (const auto& id : msg.identities) frames.push_back(id);
        frames.emplace_back(kDelimiter);
        frames.push_back(signer.sign(header, parent, metadata, content));
        frames.push_back(std::move(header));
        frames.push_back(std::move(parent));
        frames.push_back(std::move(metadata));
        frames.push_back(std::move(content));
        for (const auto& b : msg.buffers) frames.push_back(b);
        return frames;
    }

    bool decodeMessage(const std::vector<std::string>& frames,
                       const MessageSigner& signer,
                       JupyterMessage* out,
                       std::string* error) {
        auto fail = [&](const char* why) {
            if (error) *error = why;
            return false;
        };

        size_t delim = frames.size();
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i] == kDelimiter) { delim = i; break; }
        }
        if (delim == frames.size()) return fail("no <IDS|MSG> delimiter");
        // signature + four JSON frames must follow.
        if (frames.size() < delim + 6) return fail("truncated message");

        const std::string& signature = frames[delim + 1];
        const std::string& header = frames[delim + 2];
        const std::string& parent = frames[delim + 3];
        const std::string& metadata = frames[delim + 4];
        const std::string& content = frames[delim + 5];

        // Verify BEFORE parsing: an unverified message is not input we have
        // any business interpreting, and the signature covers the raw bytes.
        if (!signer.verify(signature, header, parent, metadata, content)) {
            return fail("signature does not verify");
        }

        JupyterMessage msg;
        msg.identities.assign(frames.begin(), frames.begin() + delim);

        bool ok = false;
        msg.header = dap::Json::parse(header, &ok);
        if (!ok) return fail("malformed header");
        msg.parentHeader = dap::Json::parse(parent, &ok);
        if (!ok) return fail("malformed parent_header");
        msg.metadata = dap::Json::parse(metadata, &ok);
        if (!ok) return fail("malformed metadata");
        msg.content = dap::Json::parse(content, &ok);
        if (!ok) return fail("malformed content");

        msg.buffers.assign(frames.begin() + delim + 6, frames.end());
        if (out) *out = std::move(msg);
        return true;
    }

    std::string newUuid() {
        uint64_t hi = rng()();
        uint64_t lo = rng()();
        // Version 4, variant 1.
        hi = (hi & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
        lo = (lo & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
        char buf[37];
        std::snprintf(buf, sizeof(buf),
                      "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<unsigned>(hi >> 32),
                      static_cast<unsigned>((hi >> 16) & 0xffff),
                      static_cast<unsigned>(hi & 0xffff),
                      static_cast<unsigned>(lo >> 48),
                      static_cast<unsigned long long>(lo & 0xffffffffffffULL));
        return std::string(buf);
    }

    dap::Json makeHeader(const std::string& msgType,
                         const std::string& session,
                         const std::string& username) {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto secs = system_clock::to_time_t(now);
        auto micros = duration_cast<microseconds>(now.time_since_epoch()).count()
                    % 1000000;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &secs);
#else
        gmtime_r(&secs, &tm);
#endif
        char date[64];
        std::snprintf(date, sizeof(date),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(micros));

        dap::Json h = dap::Json::object();
        h["msg_id"] = newUuid();
        h["msg_type"] = msgType;
        h["username"] = username;
        h["session"] = session;
        h["date"] = std::string(date);
        h["version"] = "5.3";
        return h;
    }

    JupyterMessage makeReply(const std::string& msgType,
                             const JupyterMessage& parent,
                             const std::string& session,
                             dap::Json content) {
        JupyterMessage reply;
        reply.identities = parent.identities;
        reply.header = makeHeader(msgType, session);
        reply.parentHeader = parent.header;
        reply.metadata = dap::Json::object();
        reply.content = std::move(content);
        return reply;
    }

}  // namespace cajeta::kernel
