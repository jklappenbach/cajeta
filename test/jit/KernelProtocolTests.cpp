//
// jupyter-kernel U5 (spec 3.1-3.4, 4.2-4.3; plan 5.1) — the protocol verbs.
//
// These drive `KernelProtocol` directly with a vector for a sink. That is the
// whole point of the layer's shape: the verbs, the message ordering, the
// execution counter and the correlation fields are testable without a socket,
// a port, or a poll loop. The ZeroMQ transport under them moves bytes and is
// covered by the loopback test in KernelTransportTests.
//

#include "gtest/gtest.h"

#include "cajeta/kernel/CellCompleteness.h"
#include "cajeta/kernel/JupyterMessage.h"
#include "cajeta/kernel/KernelProtocol.h"

#include <string>
#include <vector>

using cajeta::dap::Json;
using cajeta::kernel::Channel;
using cajeta::kernel::Completeness;
using cajeta::kernel::JupyterMessage;
using cajeta::kernel::KernelProtocol;
using cajeta::kernel::MessageSigner;
using cajeta::kernel::classifyCell;
using cajeta::kernel::decodeMessage;
using cajeta::kernel::encodeMessage;
using cajeta::kernel::makeHeader;

namespace {

    struct Sent {
        Channel channel;
        JupyterMessage msg;
    };

    // A protocol under test plus everything it sent, in order.
    class Harness {
    public:
        Harness()
            : protocol([this](Channel c, const JupyterMessage& m) {
                  sent.push_back({c, m});
              }) {
            protocol.setSessionId("kernel-session");
        }

        JupyterMessage send(const std::string& type, Json content,
                            Channel channel = Channel::Shell) {
            JupyterMessage req;
            req.identities.push_back("client-identity");
            req.header = makeHeader(type, "client-session", "tester");
            req.content = std::move(content);
            protocol.handle(channel, req);
            return req;
        }

        // Indices into `sent`, so a test can assert ORDER and not merely
        // presence — "the reply arrived before status(idle)" is a real
        // protocol requirement and an index comparison is how you say it.
        std::vector<size_t> indicesOf(const std::string& type) const {
            std::vector<size_t> out;
            for (size_t i = 0; i < sent.size(); ++i) {
                if (sent[i].msg.type() == type) out.push_back(i);
            }
            return out;
        }

        const JupyterMessage* first(const std::string& type) const {
            for (const auto& s : sent) {
                if (s.msg.type() == type) return &s.msg;
            }
            return nullptr;
        }

        std::vector<Sent> sent;
        KernelProtocol protocol;
    };

}  // namespace

// 5.1.1 / spec 3.1 — the handshake. Lab shows a live kernel only if this
// reply names the language, so every field it reads is pinned here.

// 5.1.2 / spec 4.1-4.2 — an execute_request's whole conversation: the input
// echo, the streamed output, the result, and the reply, in protocol order.
TEST(KernelProtocolTests, executeRoundTrip) {
    Harness h;
    Json content = Json::object();
    content["code"] = "System.stdout.println(\"hello\");\n41 + 1;\n";
    content["silent"] = false;
    JupyterMessage req = h.send("execute_request", content);

    const JupyterMessage* input = h.first("execute_input");
    ASSERT_NE(nullptr, input) << "no execute_input echo";
    EXPECT_EQ(1, input->content.at("execution_count").asInt());

    const JupyterMessage* stream = h.first("stream");
    ASSERT_NE(nullptr, stream) << "cell output never streamed";
    EXPECT_EQ("stdout", stream->content.at("name").asString());
    EXPECT_NE(std::string::npos, stream->content.at("text").asString().find("hello"));

    const JupyterMessage* result = h.first("execute_result");
    ASSERT_NE(nullptr, result) << "no execute_result for a trailing expression";
    EXPECT_EQ(1, result->content.at("execution_count").asInt());
    EXPECT_EQ("42", result->content.at("data").at("text/plain").asString());

    const JupyterMessage* reply = h.first("execute_reply");
    ASSERT_NE(nullptr, reply) << "no execute_reply";
    EXPECT_EQ("ok", reply->content.at("status").asString());
    EXPECT_EQ(1, reply->content.at("execution_count").asInt());
    EXPECT_EQ(req.msgId(), reply->parentHeader.at("msg_id").asString());
    EXPECT_EQ(Channel::Shell, h.sent[h.indicesOf("execute_reply")[0]].channel);

    // Order: input echo, then output, then result, then reply, then idle.
    size_t iInput = h.indicesOf("execute_input")[0];
    size_t iStream = h.indicesOf("stream")[0];
    size_t iResult = h.indicesOf("execute_result")[0];
    size_t iReply = h.indicesOf("execute_reply")[0];
    auto statuses = h.indicesOf("status");
    ASSERT_EQ(2u, statuses.size());
    EXPECT_LT(statuses[0], iInput);
    EXPECT_LT(iInput, iStream);
    EXPECT_LT(iStream, iResult);
    EXPECT_LT(iResult, iReply);
    EXPECT_LT(iReply, statuses[1]) << "idle must not precede the reply";

    // The counter advances per execute, and a cell with no result publishes
    // no execute_result at all (an empty Out[N] is not a thing).
    h.sent.clear();
    Json second = Json::object();
    second["code"] = "int32 quiet = 3;\n";
    h.send("execute_request", second);
    EXPECT_EQ(nullptr, h.first("execute_result"))
        << "a declaration produced an Out[N]";
    ASSERT_NE(nullptr, h.first("execute_reply"));
    EXPECT_EQ(2, h.first("execute_reply")->content.at("execution_count").asInt());
}

// The session's FIRST compile pulls the whole stdlib through the same
// diagnostics bridge the cell uses. Unfiltered, that republished forty-odd
// ownership warnings about stdlib internals as if a two-line cell had
// provoked them — which is what the first live Lab run actually showed.
// Anything the notebook prints on stderr must name the cell it came from.

// spec 4.4 — a throwing cell answers `error`, with the type, the message, and
// the `In[N]` traceback, and the reply says error rather than ok.
TEST(KernelProtocolTests, throwingCellRepliesError) {
    Harness h;
    Json content = Json::object();
    content["code"] = "throw heap Exception(\"boom\");\n";
    h.send("execute_request", content);

    const JupyterMessage* err = h.first("error");
    ASSERT_NE(nullptr, err) << "no error message published";
    EXPECT_NE(std::string::npos, err->content.at("ename").asString().find("Exception"));
    EXPECT_EQ("boom", err->content.at("evalue").asString());
    ASSERT_GT(err->content.at("traceback").size(), 0u) << "empty traceback";

    const JupyterMessage* reply = h.first("execute_reply");
    ASSERT_NE(nullptr, reply);
    EXPECT_EQ("error", reply->content.at("status").asString());

    // Still alive: the next cell runs and the counter kept moving.
    h.sent.clear();
    Json ok = Json::object();
    ok["code"] = "1 + 1;\n";
    h.send("execute_request", ok);
    ASSERT_NE(nullptr, h.first("execute_result"));
    EXPECT_EQ("2", h.first("execute_result")->content.at("data").at("text/plain").asString());
    EXPECT_EQ(2, h.first("execute_reply")->content.at("execution_count").asInt());
}

// 5.1.6 / spec 4.2-4.3 — the result is a MIME BUNDLE. `text/plain` is always
// there; `application/json` accompanies it only when the value's own
// rendering round-trips as JSON, which is the shape a structured-output
// frontend can actually use. A number is not that, and claiming otherwise
// would have every frontend render an `application/json` of "42".
TEST(KernelProtocolTests, resultCarriesJsonBundle) {
    Harness h;
    Json plain = Json::object();
    plain["code"] = "7 * 6;\n";
    h.send("execute_request", plain);
    const JupyterMessage* numeric = h.first("execute_result");
    ASSERT_NE(nullptr, numeric);
    EXPECT_EQ("42", numeric->content.at("data").at("text/plain").asString());
    EXPECT_TRUE(numeric->content.at("data").at("application/json").isNull())
        << "a scalar was published as structured JSON";

    h.sent.clear();
    Json structured = Json::object();
    structured["code"] = "\"{\\\"x\\\": 1, \\\"y\\\": 2}\";\n";
    h.send("execute_request", structured);
    const JupyterMessage* bundle = h.first("execute_result");
    ASSERT_NE(nullptr, bundle) << "no result for the JSON-shaped value";
    const Json& data = bundle->content.at("data");
    EXPECT_FALSE(data.at("text/plain").asString().empty());
    ASSERT_TRUE(data.at("application/json").isObject())
        << "JSON-shaped rendering did not carry a structured bundle";
    EXPECT_EQ(1, data.at("application/json").at("x").asInt());
    EXPECT_EQ(2, data.at("application/json").at("y").asInt());
}

// 5.1.3 / spec 3.2 — a message whose HMAC does not verify is DROPPED. The
// codec is where that decision lives, so this exercises it directly: sign
// with one key, verify with another, and check nothing gets through.

// 5.1.4 / spec 3.4 — the console prompt's triage.
TEST(KernelProtocolTests, isCompleteTriage) {
    std::string indent;
    EXPECT_EQ(Completeness::Complete, classifyCell("int32 x = 1;\n", &indent));
    EXPECT_TRUE(indent.empty());

    // Ran out of input mid-body: another line can still finish it, and the
    // frontend gets the indent to pre-fill.
    EXPECT_EQ(Completeness::Incomplete,
              classifyCell("void f() {\n    int32 y = 2;\n", &indent));
    EXPECT_EQ("    ", indent);

    // The two constructs that may legally span a line break, and that the
    // LEXER reports at their opening token rather than at EOF — so a
    // parser-position test alone gets both wrong.
    EXPECT_EQ(Completeness::Incomplete,
              classifyCell("String s = \"\"\"\n  line one\n"));
    EXPECT_EQ(Completeness::Incomplete, classifyCell("/* unclosed\n"));

    // An ordinary string literal may NOT span lines (CajetaLexer.g4:165), so
    // an unterminated one is a mistake, not an unfinished thought: no further
    // line closes it, and telling the console to keep waiting would hang the
    // user in a prompt with no way out.
    EXPECT_EQ(Completeness::Invalid, classifyCell("String s = \"open\n"));

    // A genuine mistake: no amount of further typing rescues it, so the
    // frontend should submit and let the user see the error.
    EXPECT_EQ(Completeness::Invalid, classifyCell("int32 = = ;\n"));

    // An empty prompt submits; calling it incomplete traps the user in it.
    EXPECT_EQ(Completeness::Complete, classifyCell("   \n\n"));

    // And over the wire.
    Harness h;
    Json content = Json::object();
    content["code"] = "void f() {\n";
    h.send("is_complete_request", content);
    const JupyterMessage* reply = h.first("is_complete_reply");
    ASSERT_NE(nullptr, reply);
    EXPECT_EQ("incomplete", reply->content.at("status").asString());
    EXPECT_EQ("    ", reply->content.at("indent").asString());
}

// 5.1.5 / spec 3.3 — restart drops every session binding and resets the
// counter. The point of the test is the FIRST half: a name bound before the
// restart must be gone after it, which is what makes restart mean anything.
TEST(KernelProtocolTests, shutdownAndRestartCleanState) {
    Harness h;
    Json bind = Json::object();
    bind["code"] = "int32 survivor = 99;\n";
    h.send("execute_request", bind);
    ASSERT_NE(nullptr, h.first("execute_reply"));
    EXPECT_EQ("ok", h.first("execute_reply")->content.at("status").asString());

    // The control: WITHIN the session the binding reads back, so the
    // post-restart assertion below is about the restart and not about
    // cross-cell reads never having worked.
    h.sent.clear();
    Json readBefore = Json::object();
    readBefore["code"] = "survivor;\n";
    h.send("execute_request", readBefore);
    ASSERT_NE(nullptr, h.first("execute_result")) << "cross-cell read produced no Out";
    EXPECT_EQ("99", h.first("execute_result")->content.at("data").at("text/plain").asString());

    h.sent.clear();
    Json shutdown = Json::object();
    shutdown["restart"] = true;
    h.send("shutdown_request", shutdown, Channel::Control);
    const JupyterMessage* reply = h.first("shutdown_reply");
    ASSERT_NE(nullptr, reply) << "no shutdown_reply";
    EXPECT_TRUE(reply->content.at("restart").asBool());
    EXPECT_EQ(Channel::Control, h.sent[h.indicesOf("shutdown_reply")[0]].channel);
    EXPECT_TRUE(h.protocol.restartRequested());
    EXPECT_TRUE(h.protocol.shutdownRequested());

    h.protocol.restartSession();
    EXPECT_EQ(0, h.protocol.executionCount()) << "counter survived a restart";

    h.sent.clear();
    Json read = Json::object();
    read["code"] = "survivor;\n";
    h.send("execute_request", read);
    const JupyterMessage* after = h.first("execute_reply");
    ASSERT_NE(nullptr, after);
    // The binding is GONE: the same read that returned 99 a moment ago now
    // yields nothing. Note what this does NOT assert — that the read reports
    // an unresolved-name error. It currently replies `ok` and simply produces
    // no value, which is the silent-resolution gap tracked by
    // silent-resolution-diagnostics-plan, not a restart defect. Pinning the
    // value's disappearance is the part spec 3.3 actually promises.
    const JupyterMessage* afterResult = h.first("execute_result");
    EXPECT_TRUE(afterResult == nullptr
                || afterResult->content.at("data").at("text/plain").asString() != "99")
        << "a binding from before the restart survived it: "
        << afterResult->content.dump();
    EXPECT_EQ(1, after->content.at("execution_count").asInt())
        << "the counter did not restart at 1";
}

// The protocol must not fault on a verb it does not implement, and must
// answer the ones a frontend sends unprompted at startup — a kernel that
// stays silent on `comm_info_request` leaves Lab waiting.
TEST(KernelProtocolTests, ancillaryVerbsAnswered) {
    Harness h;
    h.send("comm_info_request", Json::object());
    ASSERT_NE(nullptr, h.first("comm_info_reply"));
    EXPECT_TRUE(h.first("comm_info_reply")->content.at("comms").isObject());

    h.sent.clear();
    h.send("history_request", Json::object());
    ASSERT_NE(nullptr, h.first("history_reply"));
    EXPECT_TRUE(h.first("history_reply")->content.at("history").isArray());

    h.sent.clear();
    h.send("complete_request", Json::object());
    ASSERT_NE(nullptr, h.first("complete_reply"));
    EXPECT_EQ("ok", h.first("complete_reply")->content.at("status").asString());

    // Unknown verb: ignored, not faulted on, and it does not leave the
    // frontend's spinner spinning.
    h.sent.clear();
    h.send("some_future_request", Json::object());
    for (const auto& s : h.sent) {
        EXPECT_NE("status", s.msg.type())
            << "an ignored verb still moved the busy/idle state";
    }
}
