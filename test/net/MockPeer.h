//
// MockPeer.h — NET-13.3 scriptable mock peers.
//
// A **scriptable** loopback TCP server that, unlike the NET-13.1
// `LoopbackHttpServer` (which speaks just-enough-HTTP and is therefore
// *correct* by construction), is deliberately able to misbehave: it
// replays a caller-authored *script* of raw byte chunks, inter-chunk
// delays, half-closes, and aborts. That lets the HTTP/WebSocket **client
// error paths** (NET-8, NET-10) be exercised *deterministically* against
// the adversarial wire conditions a real network produces only flakily:
//
//   * **redirect chains** — a sequence of `3xx` + `Location:` responses
//     across reconnections, terminating in a `200`, so a client's
//     follow-redirect + max-hops + loop-detection logic has a fixed peer.
//   * **slow drips** — a well-formed response emitted one byte (or one
//     small chunk) at a time with a delay between, so an incremental
//     parser / read-timeout is driven across arbitrary feed splits.
//   * **malformed framing** — a header block with a bogus status line, a
//     non-numeric / negative / oversized `Content-Length`, a chunk size
//     that isn't hex, etc., so the parser's reject path fires.
//   * **premature EOF** — the peer closes (or RST-style aborts) partway
//     through the headers or partway through a promised body, so the
//     client's truncation handling (`UnexpectedEofException`) fires.
//
// ## Relationship to NET-13.1
//
// This is *not* a fork of the loopback fixture: it **subclasses**
// NET-13.1's `LoopbackServerBase` (`test/net/LoopbackFixtures.h`),
// reusing its ephemeral-127.0.0.1 bind, accept loop, background worker,
// connection counter, and the cross-platform Winsock/BSD shims verbatim.
// `MockPeer` only adds the per-connection *script* and the `serve()`
// override that plays it. So the two fixtures share one socket layer and
// agree on loopback semantics on every platform.
//
// ## Why a C++ fixture (not a `.cajeta` one)
//
// Same rationale as NET-13.1: the cajeta-surface `TcpStream` lowering is
// still being wired (NET-1.3 is "partial"), so the misbehaving *peer* a
// cajeta client connects to must be a plain host-side server that speaks
// raw sockets and has zero dependency on the cajeta.io.net surface. It lands
// now and is consumed by the later client phases — the same standalone
// posture NET-13.1 and the NET-13.2 corpus have. Kept under test/ so
// production sources carry no test-only surface.
//
// ## Scripting model
//
// A connection is served by replaying an ordered list of `Step`s. The
// fluent builder reads top-to-bottom as the wire timeline:
//
//     MockPeer peer(MockScript()
//         .send("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n")
//         .delay(20)            // 20ms drip gap
//         .send("hel")
//         .send("lo")
//         .closeWrite());       // orderly EOF (half-close)
//
// By default the same script is replayed on **every** accepted
// connection (the common case for a single-shot client test). For a
// redirect chain — where each hop is a fresh connection (the fixture
// answers `Connection: close`) — give a *sequence* of scripts; the Nth
// connection plays the Nth script (the last repeats if more connections
// arrive):
//
//     MockPeer peer({
//         MockScript().redirect(302, "/b"),   // conn 1 → 302 Location:/b
//         MockScript().redirect(302, "/c"),   // conn 2 → 302 Location:/c
//         MockScript().ok("done"),            // conn 3 → 200 done
//     });
//
// A `MockPeer` reads (and discards, capturing it for assertions) the
// client's request up to the header terminator before playing its
// script, so a client that writes a request and then reads is not
// deadlocked by a peer that writes first. `drainRequest(false)` disables
// that for tests that want the server to speak immediately.
//
#pragma once

#include "net/LoopbackFixtures.h"   // LoopbackServerBase + socket shims

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // MockScript — an ordered timeline of wire actions for one connection.
    //
    // Built fluently; each method appends a step and returns `*this`. The
    // raw `send()` is the primitive; the HTTP-flavored helpers (`status`,
    // `ok`, `redirect`, `chunked`, the malformed/truncated forms) are thin
    // sugar that append the corresponding raw bytes, so a test reads as the
    // exact wire it asserts against.
    // -----------------------------------------------------------------------
    class MockScript {
    public:
        enum class Op {
            Send,        // write `data` to the peer
            Delay,       // sleep `delayMs` (drip pacing / read-timeout tests)
            CloseWrite,  // shutdown(SHUT_WR): orderly EOF, socket still open
            Abort,       // hard close now: premature EOF mid-stream
        };

        struct Step {
            Op op;
            std::string data;   // for Send
            int delayMs = 0;    // for Delay
        };

        MockScript() = default;

        // --- primitives -----------------------------------------------------

        // Emit raw bytes exactly as given (no framing added). The building
        // block every helper below funnels through.
        MockScript& send(std::string bytes) {
            steps_.push_back({Op::Send, std::move(bytes), 0});
            return *this;
        }

        // Pause `ms` milliseconds before the next step — the drip gap a
        // read-timeout or incremental-parse test paces against.
        MockScript& delay(int ms) {
            steps_.push_back({Op::Delay, {}, ms});
            return *this;
        }

        // Orderly half-close: the client's read sees EOF after the bytes
        // sent so far. The natural terminator for a `Connection: close`
        // response (and the trigger for a *clean* end-of-body for a
        // read-to-EOF framed body).
        MockScript& closeWrite() {
            steps_.push_back({Op::CloseWrite, {}, 0});
            return *this;
        }

        // Hard abort: close the socket immediately, modeling a peer that
        // vanishes mid-stream. When this lands *before* a promised body is
        // complete it is the premature-EOF / truncation condition the
        // client's `UnexpectedEofException` path must catch.
        MockScript& abort() {
            steps_.push_back({Op::Abort, {}, 0});
            return *this;
        }

        // --- well-formed HTTP sugar ----------------------------------------

        // A complete, valid response with a Content-Length body and an
        // orderly close. The "happy peer" a redirect chain terminates in.
        MockScript& status(int code, const std::string& reason,
                           const std::string& body,
                           const std::string& contentType = "text/plain") {
            std::string head;
            head += "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
            head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            head += "Content-Type: " + contentType + "\r\n";
            head += "Connection: close\r\n\r\n";
            return send(head + body).closeWrite();
        }

        // 200 OK with `body`.
        MockScript& ok(const std::string& body,
                      const std::string& contentType = "text/plain") {
            return status(200, "OK", body, contentType);
        }

        // A redirect hop: `code` (301/302/303/307/308) + `Location:`,
        // empty body, orderly close. Chain several across connections to
        // build a redirect chain (or point one back at itself for the
        // loop-detection test).
        MockScript& redirect(int code, const std::string& location) {
            std::string reason;
            switch (code) {
                case 301: reason = "Moved Permanently"; break;
                case 302: reason = "Found"; break;
                case 303: reason = "See Other"; break;
                case 307: reason = "Temporary Redirect"; break;
                case 308: reason = "Permanent Redirect"; break;
                default:  reason = "Redirect"; break;
            }
            std::string head;
            head += "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
            head += "Location: " + location + "\r\n";
            head += "Content-Length: 0\r\n";
            head += "Connection: close\r\n\r\n";
            return send(head).closeWrite();
        }

        // A valid chunked-transfer response: each element of `chunks` is one
        // chunk (hex size + CRLF + data + CRLF), then the zero terminator.
        // The peer a streaming chunked-decoder test reads, optionally with
        // `delay()`s interleaved by the caller for a drip.
        MockScript& chunked(const std::vector<std::string>& chunks,
                          int code = 200, const std::string& reason = "OK") {
            std::string head;
            head += "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
            head += "Transfer-Encoding: chunked\r\n";
            head += "Connection: close\r\n\r\n";
            send(head);
            for (const auto& c : chunks) {
                send(hexSize(c.size()) + "\r\n" + c + "\r\n");
            }
            return send("0\r\n\r\n").closeWrite();
        }

        // --- malformed / truncated framing ---------------------------------

        // A header block whose status line is garbage (no "HTTP/x" version
        // token), so the response-head parser's reject path fires.
        MockScript& malformedStatusLine() {
            return send("NOT-HTTP garbage line\r\n"
                       "Content-Length: 0\r\n\r\n").closeWrite();
        }

        // A response whose Content-Length is non-numeric (or, with a custom
        // value, negative / absurd) — the abuse case a Content-Length
        // validator must reject rather than trust.
        MockScript& badContentLength(const std::string& value = "not-a-number") {
            return send("HTTP/1.1 200 OK\r\n"
                       "Content-Length: " + value + "\r\n"
                       "Connection: close\r\n\r\n").closeWrite();
        }

        // A chunked body whose first chunk-size token isn't valid hex — the
        // `InvalidChunkEncodingException` trigger.
        MockScript& badChunkSize(const std::string& sizeToken = "zz") {
            return send("HTTP/1.1 200 OK\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "Connection: close\r\n\r\n"
                       + sizeToken + "\r\nXX\r\n").closeWrite();
        }

        // Promise N body bytes via Content-Length but send fewer, then
        // abort — the premature-EOF-mid-body truncation case.
        MockScript& truncatedBody(int promised, const std::string& partial) {
            std::string head;
            head += "HTTP/1.1 200 OK\r\n";
            head += "Content-Length: " + std::to_string(promised) + "\r\n";
            head += "Connection: close\r\n\r\n";
            return send(head + partial).abort();
        }

        // Send a fragment of a header block, then abort before the CRLFCRLF
        // terminator — premature EOF *within the headers*.
        MockScript& truncatedHeaders(
                const std::string& partial = "HTTP/1.1 200 OK\r\nContent-Len") {
            return send(partial).abort();
        }

        const std::vector<Step>& steps() const { return steps_; }

    private:
        // Lowercase hex of a chunk size (RFC 7230 §4.1 chunk-size token).
        static std::string hexSize(size_t n) {
            if (n == 0) return "0";
            static const char* d = "0123456789abcdef";
            std::string s;
            while (n) { s.push_back(d[n & 0xf]); n >>= 4; }
            return std::string(s.rbegin(), s.rend());
        }

        std::vector<Step> steps_;
    };

    // -----------------------------------------------------------------------
    // MockPeer — a LoopbackServerBase that plays a MockScript per connection.
    //
    // Single-script ctor: every connection replays the one script. Sequence
    // ctor: the Nth connection plays the Nth script (the last repeats once
    // exhausted), so a redirect chain — one connection per hop, since each
    // response says `Connection: close` — is expressed as a list of scripts.
    // -----------------------------------------------------------------------
    class MockPeer : public LoopbackServerBase {
    public:
        explicit MockPeer(MockScript script)
            : scripts_{std::move(script)} { start(); }

        explicit MockPeer(std::vector<MockScript> scripts)
            : scripts_(std::move(scripts)) {
            if (scripts_.empty()) scripts_.emplace_back();   // never empty
            start();
        }

        ~MockPeer() override { stop(); }

        // "http://127.0.0.1:<port>" — what a Uri/HttpClient targets.
        std::string baseUrl() const {
            return "http://127.0.0.1:" + std::to_string(port());
        }

        // Whether to read+discard the client's request (to the header
        // terminator) before playing the script. On by default so a
        // request-then-read client isn't deadlocked by a write-first peer;
        // off for tests that want the server to speak first.
        MockPeer& drainRequest(bool on) {
            drainRequest_.store(on);
            return *this;
        }

        // The raw request bytes (up to the header terminator) the most
        // recent connection sent — lets a test assert the client emitted the
        // request it expected before the mock replied. Empty if drain is off.
        std::string lastRequest() {
            std::lock_guard<std::mutex> lk(mu_);
            return lastRequest_;
        }

    protected:
        void serve(cajeta_net_test_socket_t conn) override {
            // Pick this connection's script (clamped to the last one).
            size_t idx = served_.fetch_add(1);
            const MockScript& script =
                scripts_[idx < scripts_.size() ? idx : scripts_.size() - 1];

            if (drainRequest_.load()) {
                std::string req;
                char tmp[4096];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    int n = ::recv(conn, tmp, static_cast<int>(sizeof(tmp)), 0);
                    if (n <= 0) break;                 // client closed / no req
                    req.append(tmp, static_cast<size_t>(n));
                    if (req.size() > (1u << 20)) break;  // bound a hostile peer
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    lastRequest_ = req;
                }
            }

            for (const auto& step : script.steps()) {
                switch (step.op) {
                    case MockScript::Op::Send:
                        if (!sendAll(conn, step.data.data(), step.data.size()))
                            return;   // peer gone — nothing more to do
                        break;
                    case MockScript::Op::Delay:
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(step.delayMs));
                        break;
                    case MockScript::Op::CloseWrite:
                        // Orderly half-close: EOF to the client, socket still
                        // open so any in-flight client bytes still arrive.
                        ::shutdown(conn, 1 /* SHUT_WR / SD_SEND */);
                        break;
                    case MockScript::Op::Abort:
                        // Premature EOF mid-stream: shut the connection down
                        // (both directions) so the client's read sees EOF
                        // immediately, then return — the base accept loop
                        // owns the actual close() of `conn`, so we must NOT
                        // close it here (that would double-close the fd).
                        ::shutdown(conn, CAJETA_NET_TEST_SHUT_RDWR);
                        return;
                }
            }
        }

    private:
        std::vector<MockScript> scripts_;
        std::atomic<size_t> served_{0};
        std::atomic<bool> drainRequest_{true};
        std::mutex mu_;
        std::string lastRequest_;
    };

} // namespace cajeta::net::testing
