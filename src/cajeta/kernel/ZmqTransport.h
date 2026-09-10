// The Jupyter ZeroMQ transport: five sockets over three threads sharing only
// queues. The IO thread owns every protocol socket (not thread-safe), one
// execution thread owns every compile (thread_local reuse), hb echoes alone.
#pragma once

#include <memory>
#include <string>

namespace cajeta::kernel {

    // A Jupyter connection file: the frontend writes one and passes its path;
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

        // Reads and parses `path`; false, with a reason in `error`, if the file
        // is missing or is not a connection file.
        static bool load(const std::string& path, ConnectionInfo* out,
                         std::string* error = nullptr);

        // Serializes every field under its Jupyter connection-file key, the inverse
        // of load(). Emits all of them, including a defaulted port.
        std::string toJson() const;

        // Writes toJson() plus a newline to `path`, truncating any existing file.
        // False, with a reason in `error`, when the file cannot be opened.
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

        // Bind the five sockets. Ports left at 0 in `info` are chosen by the OS
        // and written back, so `info` ends up naming the ports actually in use.
        bool bind(ConnectionInfo* info, std::string* error = nullptr);

        // Run until `shutdown_request` or `stop()`; returns the exit code.
        int run();

        // Ask the loop to finish. Safe from any thread; a signal handler calls it.
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
