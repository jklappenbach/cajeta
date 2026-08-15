#include "cajeta/util/FdCapture.h"

#include <chrono>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define CAJETA_DUP _dup
#define CAJETA_DUP2 _dup2
#define CAJETA_CLOSE _close
#define CAJETA_FILENO _fileno
#else
#include <unistd.h>
#define CAJETA_DUP dup
#define CAJETA_DUP2 dup2
#define CAJETA_CLOSE close
#define CAJETA_FILENO fileno
#endif

namespace cajeta::util {

    FdCapture::FdCapture(int fd, Sink sink, int pollMs)
        : fd_(fd), sink_(std::move(sink)), pollMs_(pollMs > 0 ? pollMs : 1) {
        file_ = std::tmpfile();
        if (!file_) return;
        // Flush whatever the stream already holds, so bytes written BEFORE
        // the capture began land on the real descriptor rather than in our
        // file. Without this a buffered partial line from earlier work would
        // be attributed to this cell.
        std::FILE* stream = (fd_ == 2) ? stderr : stdout;
        std::fflush(stream);
        savedFd_ = CAJETA_DUP(fd_);
        if (savedFd_ == -1) {
            std::fclose(file_);
            file_ = nullptr;
            return;
        }
        CAJETA_DUP2(CAJETA_FILENO(file_), fd_);
        running_ = true;
        pump_ = std::thread([this] {
            while (running_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pollMs_));
                drain();
            }
        });
    }

    FdCapture::~FdCapture() {
        stop();
    }

    void FdCapture::stop() {
        if (!file_) return;
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            if (pump_.joinable()) pump_.join();
        }
        // Restore FIRST, then take the tail: anything the sink itself prints
        // must go to the real descriptor, not back into the capture.
        std::FILE* stream = (fd_ == 2) ? stderr : stdout;
        std::fflush(stream);
        if (savedFd_ != -1) {
            CAJETA_DUP2(savedFd_, fd_);
            CAJETA_CLOSE(savedFd_);
            savedFd_ = -1;
        }
        drain();
        std::fclose(file_);
        file_ = nullptr;
    }

    void FdCapture::drain() {
        if (!file_) return;
        // The writer is the C runtime writing through `fd_`; flushing the
        // stream makes its buffered bytes visible to our reads.
        std::FILE* stream = (fd_ == 2) ? stderr : stdout;
        std::fflush(stream);
        std::fflush(file_);

        if (std::fseek(file_, 0, SEEK_END) != 0) return;
        long end = std::ftell(file_);
        if (end <= readOffset_) return;

        size_t pending = static_cast<size_t>(end - readOffset_);
        if (std::fseek(file_, readOffset_, SEEK_SET) != 0) return;

        std::string chunk;
        chunk.resize(pending);
        size_t got = std::fread(&chunk[0], 1, pending, file_);
        if (got == 0) return;
        chunk.resize(got);
        readOffset_ += static_cast<long>(got);
        delivered_ += got;
        if (sink_) sink_(chunk);
    }

}  // namespace cajeta::util
