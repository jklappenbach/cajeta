//
// jupyter-kernel U5 (spec 3.1-3.2) — the Jupyter v5.3 wire message.
//
// A Jupyter message is a multipart ZeroMQ frame sequence, not a blob:
//
//   [identity...] "<IDS|MSG>" signature header parent_header metadata content [buffers...]
//
// The identity frames are ZeroMQ's own routing prefix and must be echoed back
// verbatim on a reply or the ROUTER socket cannot address the caller. The
// signature is an HMAC over the four JSON frames CONCATENATED IN ORDER —
// header, parent_header, metadata, content — computed over the exact bytes on
// the wire, which is why decode keeps the raw frames around rather than
// re-serializing the parsed DOM to verify (a round-trip through any JSON
// writer reorders keys and the signature stops matching).
//
// Nothing here knows what a kernel is. This file is the codec; KernelProtocol
// is the verbs; ZmqTransport is the sockets.
//
#pragma once

#include <string>
#include <vector>

#include "cajeta/dap/Json.h"

namespace cajeta::kernel {

    // The five channels of the protocol. Heartbeat carries no messages — it
    // echoes bytes on its own thread — so it is not one of these.
    enum class Channel { Shell, IOPub, Control, Stdin };

    const char* channelName(Channel c);

    struct JupyterMessage {
        // ZeroMQ routing prefix, verbatim. Empty on IOPub (a PUB socket has
        // no caller to address) and on anything we originate.
        std::vector<std::string> identities;
        dap::Json header = dap::Json::object();
        dap::Json parentHeader = dap::Json::object();
        dap::Json metadata = dap::Json::object();
        dap::Json content = dap::Json::object();
        // Binary attachments (v5.1+). Passed through untouched; the signature
        // does NOT cover them, per the protocol.
        std::vector<std::string> buffers;

        std::string type() const { return header.at("msg_type").asString(); }
        std::string msgId() const { return header.at("msg_id").asString(); }
    };

    // HMAC-SHA256 over the connection file's key. An EMPTY key is the
    // protocol's explicit "unsigned" mode: the signature frame is present but
    // empty, and verification accepts anything. That is a real Jupyter
    // configuration, not a test affordance.
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

        // Constant-time compare against a received signature. Always true in
        // unsigned mode.
        bool verify(const std::string& signature,
                    const std::string& header,
                    const std::string& parentHeader,
                    const std::string& metadata,
                    const std::string& content) const;

    private:
        std::string key_;
        std::string scheme_ = "hmac-sha256";
    };

    // Serialize to wire frames, signing as it goes.
    std::vector<std::string> encodeMessage(const JupyterMessage& msg,
                                           const MessageSigner& signer);

    // Parse wire frames. Returns false — with a reason in `error` when
    // non-null — on a missing delimiter, a malformed JSON frame, or a
    // signature that does not verify. Spec 3.2: the caller DROPS the message
    // on false; it never faults and never replies.
    bool decodeMessage(const std::vector<std::string>& frames,
                       const MessageSigner& signer,
                       JupyterMessage* out,
                       std::string* error = nullptr);

    // Build a header for a message we originate: fresh msg_id, ISO-8601 UTC
    // date, the session's id and username, protocol version 5.3.
    dap::Json makeHeader(const std::string& msgType,
                         const std::string& session,
                         const std::string& username = "kernel");

    // A reply carrying `parent`'s header as its parent_header and, on a
    // ROUTER channel, its identities — the two things a frontend correlates
    // on. Everything the kernel sends in response to a request goes through
    // here so neither can be forgotten.
    JupyterMessage makeReply(const std::string& msgType,
                             const JupyterMessage& parent,
                             const std::string& session,
                             dap::Json content);

    // RFC 4122 v4, lowercase, hyphenated.
    std::string newUuid();

}  // namespace cajeta::kernel
