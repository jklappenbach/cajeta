// jupyter-kernel U5 - the Jupyter v5.3 wire message codec. Frame order is
//   [identity...] "<IDS|MSG>" signature header parent_header metadata content [buffers...]
// and the signature covers the four JSON frames' EXACT wire bytes, so decode keeps them raw.
#pragma once

#include <string>
#include <vector>

#include "cajeta/dap/Json.h"

namespace cajeta::kernel {

    // The five channels of the protocol; heartbeat carries no messages, so it is absent.
    enum class Channel { Shell, IOPub, Control, Stdin };

    const char* channelName(Channel c);

    struct JupyterMessage {
        // ZeroMQ routing prefix, verbatim. Empty on IOPub and on anything we originate.
        std::vector<std::string> identities;
        dap::Json header = dap::Json::object();
        dap::Json parentHeader = dap::Json::object();
        dap::Json metadata = dap::Json::object();
        dap::Json content = dap::Json::object();
        // Binary attachments (v5.1+), passed through; the signature does NOT cover them.
        std::vector<std::string> buffers;

        std::string type() const { return header.at("msg_type").asString(); }
        std::string msgId() const { return header.at("msg_id").asString(); }
    };

    // HMAC-SHA256 over the connection file's key. An EMPTY key is the protocol's explicit
    // "unsigned" mode: the signature frame is present but empty and verification accepts all.
    class MessageSigner {
    public:
        MessageSigner() = default;
        explicit MessageSigner(std::string key,
                               std::string scheme = "hmac-sha256");

        bool enabled() const { return !key_.empty(); }
        const std::string& scheme() const { return scheme_; }

        // Lowercase hex digest over the four frames in protocol order.
        std::string sign(const std::string& header,
                         const std::string& parentHeader,
                         const std::string& metadata,
                         const std::string& content) const;

        // Constant-time compare against a received signature; always true unsigned.
        bool verify(const std::string& signature,
                    const std::string& header,
                    const std::string& parentHeader,
                    const std::string& metadata,
                    const std::string& content) const;

    private:
        std::string key_;
        std::string scheme_ = "hmac-sha256";
    };

    std::vector<std::string> encodeMessage(const JupyterMessage& msg,
                                           const MessageSigner& signer);

    // Parse wire frames. False, with a reason in `error` when non-null, on a missing
    // delimiter, malformed JSON, or a bad signature; the caller then DROPS the message.
    bool decodeMessage(const std::vector<std::string>& frames,
                       const MessageSigner& signer,
                       JupyterMessage* out,
                       std::string* error = nullptr);

    dap::Json makeHeader(const std::string& msgType,
                         const std::string& session,
                         const std::string& username = "kernel");

    // A reply carrying `parent`'s header as its parent_header and, on a ROUTER channel,
    // its identities - the two things a frontend correlates on, so nothing may skip it.
    JupyterMessage makeReply(const std::string& msgType,
                             const JupyterMessage& parent,
                             const std::string& session,
                             dap::Json content);

    // RFC 4122 v4, lowercase, hyphenated.
    std::string newUuid();

}  // namespace cajeta::kernel
