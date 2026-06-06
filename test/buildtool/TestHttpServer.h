// Tiny localhost HTTP/1.1 server used by HttpRepository tests
// (Phase 6b, 6d). Speaks just enough HTTP to round-trip GET + POST
// with canned responses. Body read for POST so curl can complete
// the upload. No chunked encoding, no keep-alive games.
//
// Kept under test/ so production sources have no test-only surface.

#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

// Berkeley sockets on POSIX; the (largely API-compatible) Winsock2 surface on
// Windows. The small shims below paper over the handful of real differences:
// the socket handle type, the close/shutdown spellings, and the invalid-handle
// sentinel (Windows SOCKET is unsigned, so the POSIX `< 0` checks don't work).
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
using cajeta_socket_t = SOCKET;
#  define CAJETA_CLOSESOCK closesocket
#  define CAJETA_SHUT_RDWR SD_BOTH
#  define CAJETA_BAD_SOCKET INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using cajeta_socket_t = int;
#  define CAJETA_CLOSESOCK ::close
#  define CAJETA_SHUT_RDWR SHUT_RDWR
#  define CAJETA_BAD_SOCKET (-1)
#endif

namespace cajeta::buildtool::testing {

#if defined(_WIN32)
    // Process-wide Winsock init/teardown, done once on first server use.
    inline void ensureWinsock() {
        static struct WinsockGuard {
            WinsockGuard() { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); }
            ~WinsockGuard() { ::WSACleanup(); }
        } guard;
    }
#endif

    class TestHttpServer {
    public:
        struct Response {
            int status = 200;
            std::string body;
            // Optional override of the default Content-Type
            // (application/json). v2 endpoints serve other types
            // (tar.zst bundles, raw blob bytes).
            std::string contentType;
        };

        TestHttpServer() {
#if defined(_WIN32)
            ensureWinsock();
#endif
            sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock_ == CAJETA_BAD_SOCKET) {
                throw std::runtime_error("socket() failed");
            }
            int one = 1;
            ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;  // ephemeral
            if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) < 0) {
                throw std::runtime_error(
                    std::string("bind() failed: ") + std::strerror(errno));
            }
            socklen_t len = sizeof(addr);
            if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr),
                              &len) < 0) {
                throw std::runtime_error("getsockname() failed");
            }
            port_ = ntohs(addr.sin_port);
            if (::listen(sock_, 8) < 0) {
                throw std::runtime_error("listen() failed");
            }
            running_ = true;
            worker_ = std::thread([this]() { run(); });
        }

        ~TestHttpServer() { stop(); }

        std::string baseUrl() const {
            return "http://127.0.0.1:" + std::to_string(port_);
        }

        // Register a route that responds the same regardless of
        // HTTP method.
        void route(const std::string& path,
                   int status, std::string body,
                   std::string contentType = "application/json") {
            std::lock_guard<std::mutex> lk(mu_);
            routes_[path] = {status, std::move(body),
                             std::move(contentType)};
        }

        // Replace the response for `path` mid-test (handy for the
        // capability-probe TTL test that bumps the etag).
        void retarget(const std::string& path,
                      int status, std::string body,
                      std::string contentType = "application/json") {
            route(path, status, std::move(body), std::move(contentType));
        }

        std::string lastAuthHeader() {
            std::lock_guard<std::mutex> lk(mu_);
            return lastAuth_;
        }

        // Capture the most recent POST body for a path.
        std::string lastBody(const std::string& path) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = lastBodies_.find(path);
            return it == lastBodies_.end() ? std::string{} : it->second;
        }

        // Count requests to a path — useful for asserting the driver
        // didn't make extra calls or hit a cache.
        int hitCount(const std::string& path) {
            std::lock_guard<std::mutex> lk(mu_);
            return hits_[path];
        }

        // Reset hit + body capture state without clearing routes.
        void resetHits() {
            std::lock_guard<std::mutex> lk(mu_);
            hits_.clear();
            lastBodies_.clear();
        }

        void stop() {
            if (!running_.exchange(false)) return;
            ::shutdown(sock_, CAJETA_SHUT_RDWR);
            CAJETA_CLOSESOCK(sock_);
            sock_ = CAJETA_BAD_SOCKET;
            if (worker_.joinable()) worker_.join();
        }

    private:
        void run() {
            while (running_) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                cajeta_socket_t conn = ::accept(sock_,
                                    reinterpret_cast<sockaddr*>(&caddr),
                                    &clen);
                if (conn == CAJETA_BAD_SOCKET) {
                    if (!running_) break;
                    continue;
                }
                handle(conn);
                CAJETA_CLOSESOCK(conn);
            }
        }

        // Find Content-Length value in a buffer of headers (case-
        // insensitive header lookup). Returns -1 when absent.
        static long contentLengthOf(const std::string& buf,
                                    size_t headersEnd) {
            std::string lower = buf.substr(0, headersEnd);
            for (auto& c : lower) c = std::tolower(c);
            auto p = lower.find("\r\ncontent-length:");
            if (p == std::string::npos) return -1;
            auto valStart = buf.find(':', p + 2) + 1;
            while (valStart < buf.size() && buf[valStart] == ' ')
                ++valStart;
            auto valEnd = buf.find("\r\n", valStart);
            try {
                return std::stol(buf.substr(valStart, valEnd - valStart));
            } catch (...) {
                return -1;
            }
        }

        void handle(cajeta_socket_t conn) {
            std::string buf;
            char tmp[4096];
            // Read until end of headers (CRLFCRLF).
            while (buf.find("\r\n\r\n") == std::string::npos) {
                int n = ::recv(conn, tmp, static_cast<int>(sizeof(tmp)), 0);
                if (n <= 0) return;
                buf.append(tmp, static_cast<size_t>(n));
                if (buf.size() > (1 << 20)) return;
            }
            size_t headersEnd = buf.find("\r\n\r\n") + 4;
            long contentLength = contentLengthOf(buf, headersEnd);

            // Drain the body for POST so curl considers the upload
            // complete and we can capture the bytes.
            std::string body;
            if (contentLength > 0) {
                body = buf.substr(headersEnd);
                while (static_cast<long>(body.size()) < contentLength) {
                    int n = ::recv(conn, tmp, static_cast<int>(sizeof(tmp)), 0);
                    if (n <= 0) break;
                    body.append(tmp, static_cast<size_t>(n));
                }
                if (static_cast<long>(body.size()) > contentLength) {
                    body.resize(static_cast<size_t>(contentLength));
                }
            }

            // Parse request line: "GET <path> HTTP/1.1\r\n"
            auto firstLineEnd = buf.find("\r\n");
            std::string requestLine = buf.substr(0, firstLineEnd);
            std::string method, path;
            {
                std::istringstream ss(requestLine);
                ss >> method >> path;
            }

            // Capture Authorization header if present.
            std::string auth;
            {
                std::string lower = buf.substr(0, headersEnd);
                for (auto& c : lower) c = std::tolower(c);
                auto p = lower.find("\r\nauthorization:");
                if (p != std::string::npos) {
                    auto valStart = buf.find(':', p + 2) + 1;
                    while (valStart < buf.size() && buf[valStart] == ' ')
                        ++valStart;
                    auto valEnd = buf.find("\r\n", valStart);
                    auth = buf.substr(valStart, valEnd - valStart);
                }
            }

            Response resp{404, "not found", "application/json"};
            {
                std::lock_guard<std::mutex> lk(mu_);
                lastAuth_ = auth;
                ++hits_[path];
                if (!body.empty()) lastBodies_[path] = body;
                auto it = routes_.find(path);
                if (it != routes_.end()) resp = it->second;
            }

            std::string reason =
                (resp.status >= 200 && resp.status < 300) ? "OK" :
                (resp.status == 404)                       ? "Not Found" :
                (resp.status == 401)                       ? "Unauthorized" :
                                                              "Status";
            std::string ct = resp.contentType.empty()
                                 ? "application/json"
                                 : resp.contentType;
            std::ostringstream os;
            os << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
               << "Content-Length: " << resp.body.size() << "\r\n"
               << "Content-Type: " << ct << "\r\n"
               << "Connection: close\r\n"
               << "\r\n"
               << resp.body;
            std::string out = os.str();
            const char* p = out.data();
            size_t rem = out.size();
            while (rem > 0) {
                int n = ::send(conn, p, static_cast<int>(rem), 0);
                if (n <= 0) break;
                p += n;
                rem -= static_cast<size_t>(n);
            }
        }

        cajeta_socket_t sock_ = CAJETA_BAD_SOCKET;
        int port_ = 0;
        std::atomic<bool> running_{false};
        std::thread worker_;
        std::mutex mu_;
        std::unordered_map<std::string, Response> routes_;
        std::unordered_map<std::string, int> hits_;
        std::unordered_map<std::string, std::string> lastBodies_;
        std::string lastAuth_;
    };

} // namespace cajeta::buildtool::testing
