//
// LoopbackFixtures.h — NET-13.1 loopback test fixtures.
//
// An in-test TCP echo + HTTP request/response server harness — the
// `cajeta.net` analog of the build tool's TestHttpServer
// (test/buildtool/TestHttpServer.h) — so the client + server
// networking phases (NET-8 HTTP client, NET-9 HTTP server, NET-10
// WebSocket, and the NET-3 reactor harness) can test against a **real
// peer over loopback** rather than a mock.
//
// Two reusable, header-only fixtures live here:
//
//   * `TcpEchoServer`     — a bare TCP server that echoes every byte it
//                           receives, on a background thread, bound to an
//                           ephemeral 127.0.0.1 port. The foundation a
//                           socket/reactor round-trip test connects to.
//   * `LoopbackHttpServer`— a tiny HTTP/1.1 server with registered
//                           routes, canned responses, request-hit counts,
//                           and captured POST bodies. The peer the HTTP
//                           client phase (NET-8) fetches from. It is the
//                           direct descendant of the build tool's
//                           TestHttpServer, re-homed into the
//                           `cajeta::net::testing` namespace and trimmed to
//                           the cajeta.net surface the plan calls for.
//
// ## Why a C++ fixture (not a `.cajeta` one)
//
// NET-13.1 depends on NET-1.3 (`TcpStream`) and NET-1.4 (`TcpListener`),
// whose **native** intrinsics (`__cajeta_net_*`, NET-1.1) exist and round
// trip over loopback today (see NetSocketTests / NetListenerTests). The
// cajeta-surface lowering for those types is still being wired
// (the plan marks NET-1.3/1.5 "partial: needs compiler-core net
// dispatch"), so the *peer* a cajeta test connects to must be a plain
// host-side server. That is exactly the role the build tool's
// TestHttpServer plays for the build-tool tests, and the role this
// header plays for the net tests: it speaks raw BSD/Winsock sockets, has
// zero dependency on the cajeta.net surface, and therefore lands now and
// is consumed by every later phase — the same standalone posture the
// NET-13.2 golden corpus has.
//
// Kept under test/ so production sources carry no test-only surface
// (the TestHttpServer precedent).
//
#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Berkeley sockets on POSIX; the (largely API-compatible) Winsock2
// surface on Windows. The small shims below paper over the handful of
// real differences: the socket handle type, the close/shutdown
// spellings, and the invalid-handle sentinel (a Windows SOCKET is
// unsigned, so the POSIX `< 0` checks don't work). This mirrors the
// abstraction the product's cajeta_net_socket.c uses, so the fixture
// and the runtime agree on loopback semantics on both platforms.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using cajeta_net_test_socket_t = SOCKET;
#  define CAJETA_NET_TEST_CLOSESOCK closesocket
#  define CAJETA_NET_TEST_SHUT_RDWR SD_BOTH
#  define CAJETA_NET_TEST_BAD_SOCKET INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using cajeta_net_test_socket_t = int;
#  define CAJETA_NET_TEST_CLOSESOCK ::close
#  define CAJETA_NET_TEST_SHUT_RDWR SHUT_RDWR
#  define CAJETA_NET_TEST_BAD_SOCKET (-1)
#endif

namespace cajeta::net::testing {

#if defined(_WIN32)
    // Process-wide Winsock init, done once on first fixture use. The
    // product runtime's own WSAStartup (cajeta_net_socket.c) is
    // refcounted by Winsock, so a second startup here is harmless and
    // keeps the fixture self-contained when used without the runtime.
    inline void ensureWinsock() {
        static struct WinsockGuard {
            WinsockGuard() { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); }
            ~WinsockGuard() { ::WSACleanup(); }
        } guard;
    }
#endif

    // -----------------------------------------------------------------------
    // Shared base: bind an ephemeral loopback listener + run an accept loop
    // on a background thread. Subclasses implement `serve(conn)` per accepted
    // connection. Construction binds + listens + spawns the worker so the
    // server is live the instant the ctor returns; `port()` is then valid.
    // -----------------------------------------------------------------------
    class LoopbackServerBase {
    public:
        LoopbackServerBase() {
#if defined(_WIN32)
            ensureWinsock();
#endif
            sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock_ == CAJETA_NET_TEST_BAD_SOCKET) {
                throw std::runtime_error("LoopbackServer: socket() failed");
            }
            int one = 1;
            ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;  // ephemeral — kernel picks the port
            if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) < 0) {
                throw std::runtime_error("LoopbackServer: bind() failed");
            }
            socklen_t len = sizeof(addr);
            if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr),
                              &len) < 0) {
                throw std::runtime_error("LoopbackServer: getsockname() failed");
            }
            port_ = ntohs(addr.sin_port);
            if (::listen(sock_, 16) < 0) {
                throw std::runtime_error("LoopbackServer: listen() failed");
            }
        }

        // The accept loop is started by `start()` rather than the base ctor
        // because the worker calls the virtual `serve()` — which is not yet
        // overridden while the base ctor runs. Each concrete fixture's ctor
        // calls `start()` last (after its own members are constructed).
        virtual ~LoopbackServerBase() { stop(); }

        // The ephemeral loopback port the kernel assigned. Valid after
        // construction. Tests connect a cajeta client (or a raw socket) here.
        uint16_t port() const { return port_; }

        // Convenience: "127.0.0.1:<port>" — the SocketAddress.parse form the
        // cajeta layer consumes.
        std::string address() const {
            return "127.0.0.1:" + std::to_string(port_);
        }

        // Total accepted connections so far (lifetime). Lets a keep-alive
        // test assert one socket was reused across two requests by checking
        // this stays 1, the way TestHttpServer's hit-count does for routes.
        int acceptedConnections() const { return accepted_.load(); }

        // Stop accepting + join the worker. Idempotent. Shutting the
        // listening socket down unblocks the accept() the worker is parked in.
        void stop() {
            if (!running_.exchange(false)) return;
            if (sock_ != CAJETA_NET_TEST_BAD_SOCKET) {
                ::shutdown(sock_, CAJETA_NET_TEST_SHUT_RDWR);
                CAJETA_NET_TEST_CLOSESOCK(sock_);
                sock_ = CAJETA_NET_TEST_BAD_SOCKET;
            }
            if (worker_.joinable()) worker_.join();
        }

    protected:
        // Concrete fixtures call this at the end of their ctor to go live.
        void start() {
            running_ = true;
            worker_ = std::thread([this]() { acceptLoop(); });
        }

        // Per-connection handler — runs on the worker thread, owns `conn`
        // until it returns (the loop closes it afterward).
        virtual void serve(cajeta_net_test_socket_t conn) = 0;

        // Read exactly `n` bytes from `conn` into `out`, looping over short
        // reads; returns false on EOF/error before `n` bytes arrive. Shared
        // by the HTTP body-drain path and any subclass needing framed reads.
        static bool recvExactly(cajeta_net_test_socket_t conn,
                                size_t n, std::string& out) {
            char tmp[4096];
            while (out.size() < n) {
                size_t want = n - out.size();
                int chunk = want > sizeof(tmp) ? (int) sizeof(tmp) : (int) want;
                int r = ::recv(conn, tmp, chunk, 0);
                if (r <= 0) return false;
                out.append(tmp, static_cast<size_t>(r));
            }
            return true;
        }

        // Send the whole buffer, looping over short writes; false on error.
        static bool sendAll(cajeta_net_test_socket_t conn,
                            const char* p, size_t len) {
            while (len > 0) {
                int n = ::send(conn, p, static_cast<int>(len), 0);
                if (n <= 0) return false;
                p += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

    private:
        void acceptLoop() {
            while (running_.load()) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                cajeta_net_test_socket_t conn =
                    ::accept(sock_, reinterpret_cast<sockaddr*>(&caddr), &clen);
                if (conn == CAJETA_NET_TEST_BAD_SOCKET) {
                    if (!running_.load()) break;  // closed by stop()
                    continue;                      // transient — keep serving
                }
                accepted_.fetch_add(1);
                serve(conn);
                CAJETA_NET_TEST_CLOSESOCK(conn);
            }
        }

        cajeta_net_test_socket_t sock_ = CAJETA_NET_TEST_BAD_SOCKET;
        uint16_t port_ = 0;
        std::atomic<bool> running_{false};
        std::atomic<int> accepted_{0};
        std::thread worker_;
    };

    // -----------------------------------------------------------------------
    // TcpEchoServer — echoes every byte received, until the peer closes (EOF)
    // or half-closes its write side. The minimal real peer a TcpStream
    // round-trip test (and the NET-3 reactor harness) connects to: send N
    // bytes, read N identical bytes back.
    // -----------------------------------------------------------------------
    class TcpEchoServer : public LoopbackServerBase {
    public:
        TcpEchoServer() { start(); }
        ~TcpEchoServer() override { stop(); }

    protected:
        void serve(cajeta_net_test_socket_t conn) override {
            char buf[4096];
            for (;;) {
                int n = ::recv(conn, buf, static_cast<int>(sizeof(buf)), 0);
                if (n <= 0) return;          // 0 = orderly close (EOF); <0 = err
                if (!sendAll(conn, buf, static_cast<size_t>(n))) return;
            }
        }
    };

    // -----------------------------------------------------------------------
    // LoopbackHttpServer — a tiny HTTP/1.1 server with registered routes,
    // canned responses, per-path hit counts, and captured request bodies.
    // The peer the HTTP client phase (NET-8) tests against over loopback.
    //
    // Speaks just enough HTTP to round-trip GET + POST with a Content-Length
    // body. `Connection: close` per response (keep-alive framing is a later
    // server concern); an unregistered path returns 404. This is the
    // TestHttpServer contract re-homed to cajeta.net and pared to the
    // request/response shape the plan's NET-13.1 calls for.
    // -----------------------------------------------------------------------
    class LoopbackHttpServer : public LoopbackServerBase {
    public:
        struct Response {
            int status = 200;
            std::string body;
            std::string contentType;   // empty → "text/plain"
        };

        LoopbackHttpServer() { start(); }
        ~LoopbackHttpServer() override { stop(); }

        // The "http://127.0.0.1:<port>" base a Uri/HttpClient targets.
        std::string baseUrl() const {
            return "http://127.0.0.1:" + std::to_string(port());
        }

        // Register (or replace) the canned response for `path`, regardless of
        // method. Thread-safe against the worker thread.
        void route(const std::string& path, int status, std::string body,
                   std::string contentType = "text/plain") {
            std::lock_guard<std::mutex> lk(mu_);
            routes_[path] = {status, std::move(body), std::move(contentType)};
        }

        // Times `path` was requested — lets a test assert no extra calls / a
        // cache hit / a keep-alive reuse the way TestHttpServer does.
        int hitCount(const std::string& path) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = hits_.find(path);
            return it == hits_.end() ? 0 : it->second;
        }

        // The most recent POST body captured for `path` (empty if none).
        std::string lastBody(const std::string& path) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = lastBodies_.find(path);
            return it == lastBodies_.end() ? std::string{} : it->second;
        }

    protected:
        void serve(cajeta_net_test_socket_t conn) override {
            std::string buf;
            char tmp[4096];
            // Read until the header terminator (CRLFCRLF), capping the buffer
            // so a malformed peer can't grow it unbounded.
            while (buf.find("\r\n\r\n") == std::string::npos) {
                int n = ::recv(conn, tmp, static_cast<int>(sizeof(tmp)), 0);
                if (n <= 0) return;
                buf.append(tmp, static_cast<size_t>(n));
                if (buf.size() > (1u << 20)) return;
            }
            size_t headersEnd = buf.find("\r\n\r\n") + 4;
            long contentLength = contentLengthOf(buf, headersEnd);

            // Drain a Content-Length body so a POST upload completes and the
            // bytes are captured.
            std::string body = buf.substr(headersEnd);
            if (contentLength > 0) {
                if (static_cast<long>(body.size()) < contentLength) {
                    std::string rest;
                    recvExactly(conn,
                                static_cast<size_t>(contentLength) - body.size(),
                                rest);
                    body += rest;
                }
                if (static_cast<long>(body.size()) > contentLength) {
                    body.resize(static_cast<size_t>(contentLength));
                }
            } else {
                body.clear();
            }

            // Request line: "<METHOD> <path> HTTP/1.1".
            std::string requestLine = buf.substr(0, buf.find("\r\n"));
            std::string method, path;
            {
                std::istringstream ss(requestLine);
                ss >> method >> path;
            }

            Response resp{404, "not found", "text/plain"};
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++hits_[path];
                if (!body.empty()) lastBodies_[path] = body;
                auto it = routes_.find(path);
                if (it != routes_.end()) resp = it->second;
            }

            std::string reason = reasonPhrase(resp.status);
            std::string ct = resp.contentType.empty() ? "text/plain"
                                                       : resp.contentType;
            std::ostringstream os;
            os << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
               << "Content-Length: " << resp.body.size() << "\r\n"
               << "Content-Type: " << ct << "\r\n"
               << "Connection: close\r\n"
               << "\r\n"
               << resp.body;
            std::string out = os.str();
            sendAll(conn, out.data(), out.size());
        }

    private:
        // The HTTP reason phrase for the status codes the fixture serves.
        // Kept small — a test registers the statuses it needs; unknown codes
        // fall through to a generic phrase rather than asserting.
        static const char* reasonPhrase(int status) {
            switch (status) {
                case 200: return "OK";
                case 201: return "Created";
                case 204: return "No Content";
                case 301: return "Moved Permanently";
                case 302: return "Found";
                case 303: return "See Other";
                case 304: return "Not Modified";
                case 307: return "Temporary Redirect";
                case 308: return "Permanent Redirect";
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 500: return "Internal Server Error";
                default:  return "Status";
            }
        }

        // Case-insensitive Content-Length lookup in the header block; -1 if
        // absent or unparseable. (Same helper TestHttpServer carries.)
        static long contentLengthOf(const std::string& buf, size_t headersEnd) {
            std::string lower = buf.substr(0, headersEnd);
            for (auto& c : lower) c = static_cast<char>(std::tolower(c));
            auto p = lower.find("\r\ncontent-length:");
            if (p == std::string::npos) return -1;
            auto valStart = buf.find(':', p + 2) + 1;
            while (valStart < buf.size() && buf[valStart] == ' ') ++valStart;
            auto valEnd = buf.find("\r\n", valStart);
            try {
                return std::stol(buf.substr(valStart, valEnd - valStart));
            } catch (...) {
                return -1;
            }
        }

        std::mutex mu_;
        std::unordered_map<std::string, Response> routes_;
        std::unordered_map<std::string, int> hits_;
        std::unordered_map<std::string, std::string> lastBodies_;
    };

} // namespace cajeta::net::testing
