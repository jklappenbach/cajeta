// Live capture of a file descriptor, drained by a pump thread so output reaches
// a consumer WHILE the writer runs. Backed by a temp file, not a pipe: a pipe's
// fixed buffer would deadlock a large burst if the reader ever stalled.
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace cajeta::util {

    class FdCapture {
    public:
        // Called with each chunk on the PUMP thread, so it must be safe off the
        // constructing thread and must not block for long.
        using Sink = std::function<void(const std::string&)>;

        // Begins capturing `fd` immediately, polling every `pollMs`; the tail is
        // always drained by stop(), so a sub-interval capture loses nothing.
        FdCapture(int fd, Sink sink, int pollMs = 5);
        ~FdCapture();

        FdCapture(const FdCapture&) = delete;
        FdCapture& operator=(const FdCapture&) = delete;

        // Restore the descriptor and deliver the tail. Idempotent; ~FdCapture calls it.
        void stop();

        size_t bytesDelivered() const { return delivered_; }

    private:
        void drain();

        int fd_ = -1;
        int savedFd_ = -1;
        std::FILE* file_ = nullptr;
        long readOffset_ = 0;
        size_t delivered_ = 0;
        Sink sink_;
        std::atomic<bool> running_{false};
        int pollMs_ = 5;
        std::thread pump_;
    };

}  // namespace cajeta::util
