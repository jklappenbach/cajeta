//
// jupyter-kernel U5 (spec 3.1-3.3) — the ZeroMQ transport.
//
// Five sockets, three threads, and a strict rule about which thread touches
// what:
//
//   IO thread         owns EVERY protocol socket. ZeroMQ sockets are not
//                     thread-safe, so this is the only thread that ever calls
//                     zmq_msg_send/recv on shell, control, iopub or stdin.
//                     It polls for inbound frames and drains an outbound
//                     queue on each turn.
//   Execution thread  owns the KernelProtocol and, through it, the JIT
//                     session. The compiler's reuse core keeps thread_local
//                     baselines (StdlibReuseCore.h:11-15), so every cell
//                     compile must happen on ONE thread for the session's
//                     whole life — this one.
//   Heartbeat thread  owns the hb socket alone and echoes bytes. It never
//                     touches the protocol, which is the point: it answers
//                     while a cell is running, so a long cell does not read
//                     as a dead kernel.
//
// Messages cross between the first two through queues, never through shared
// state. The cell-output pump thread reaches the IO thread the same way, by
// enqueueing onto the outbound queue.
//
#pragma once

#include <memory>
#include <string>

namespace cajeta::kernel {

    // A Jupyter connection file. The frontend writes one and passes its path;
    // `cajeta kernel` with no path generates one and prints it.
    struct ConnectionInfo {
        std::string transport = "tcp";
        std::string ip = "127.0.0.1";
        int shellPort = 0;
        int iopubPort = 0;
        int stdinPort = 0;
        int controlPort = 0;
        int hbPort = 0;
        std::string key;
        std::string signatureScheme = "hmac-sha256";
        std::string kernelName = "cajeta";

        // Reads and parses `path`. False (with a reason in `error`) if the
        // file is missing or not a connection file.
        static bool load(const std::string& path, ConnectionInfo* out,
                         std::string* error = nullptr);

        std::string toJson() const;
        bool write(const std::string& path, std::string* error = nullptr) const;

        // `tcp://127.0.0.1:9000` for a given port.
        std::string endpoint(int port) const;
    };

    class KernelTransport {
    public:
        KernelTransport();
        ~KernelTransport();
        KernelTransport(const KernelTransport&) = delete;
        KernelTransport& operator=(const KernelTransport&) = delete;

        // Bind the five sockets. Ports left at 0 in `info` are chosen by the
        // OS and written back, so a generated connection file reports the
        // ports actually in use rather than ones we hoped were free.
        bool bind(ConnectionInfo* info, std::string* error = nullptr);

        // Run until `shutdown_request` (or `stop()`). Returns the exit code:
        // 0 for a clean shutdown.
        int run();

        // Ask the loop to finish. Safe from any thread — a signal handler
        // calls it.
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
