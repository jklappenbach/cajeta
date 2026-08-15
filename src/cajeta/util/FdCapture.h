//
// jupyter-kernel U3 (spec 4.1) — live capture of a file descriptor.
//
// A notebook cell's output has to reach the frontend WHILE the cell runs: a
// loop printing progress for thirty seconds shows each line as it is written,
// not a burst at the end. The cell itself runs synchronously on the session's
// execution thread, so something else has to do the draining — hence the pump
// thread here.
//
// Backed by a TEMP FILE rather than a pipe, which is the same choice
// SystemIoTests made and for the same two reasons: a pipe has a fixed buffer,
// so a cell printing a megabyte would block forever on a full pipe if the
// reader ever stalled (plan 3.3.1 exists to pin exactly that), and the
// non-blocking pipe machinery differs across platforms. A file has no such
// limit, and the pump simply reads whatever has been appended since last time.
// Ordering is trivially preserved because there is one file read forward.
//
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
        // Called with each chunk as it is drained, on the PUMP thread — so a
        // sink must be safe to call from a thread other than the one that
        // constructed the capture, and should not block for long.
        using Sink = std::function<void(const std::string&)>;

        // Begins capturing `fd` (1 = stdout, 2 = stderr) immediately.
        // `pollMs` is how often the pump looks for new bytes; the tail is
        // always drained by stop()/the destructor, so nothing is lost even if
        // the capture is shorter than one interval.
        FdCapture(int fd, Sink sink, int pollMs = 5);
        ~FdCapture();

        FdCapture(const FdCapture&) = delete;
        FdCapture& operator=(const FdCapture&) = delete;

        // Restore the descriptor and deliver whatever is left. Idempotent —
        // the destructor calls it.
        void stop();

        // Total bytes delivered to the sink so far.
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
