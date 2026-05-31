#include "cajeta/dbg/DebugController.h"

namespace cajeta::dbg {

    void DebugController::arm(int32_t locId) {
        std::lock_guard<std::mutex> lock(mutex);
        armed.insert(locId);
    }

    void DebugController::disarm(int32_t locId) {
        std::lock_guard<std::mutex> lock(mutex);
        armed.erase(locId);
    }

    void DebugController::clearArmed() {
        std::lock_guard<std::mutex> lock(mutex);
        armed.clear();
    }

    bool DebugController::isArmed(int32_t locId) const {
        std::lock_guard<std::mutex> lock(mutex);
        return armed.count(locId) != 0;
    }

    void DebugController::onSafepoint(int32_t locId, long fiberId) {
        std::unique_lock<std::mutex> lock(mutex);
        if (armed.count(locId) == 0) return;

        // Park: publish the stop, wake the debugger thread, wait for resume.
        stopped = true;
        resumeRequested = false;
        current = StopEvent{locId, fiberId};
        stoppedCv.notify_all();
        resumeCv.wait(lock, [this] { return resumeRequested; });
        stopped = false;
    }

    StopEvent DebugController::waitForStop() {
        std::unique_lock<std::mutex> lock(mutex);
        stoppedCv.wait(lock, [this] { return stopped; });
        return current;
    }

    void DebugController::resume() {
        std::lock_guard<std::mutex> lock(mutex);
        resumeRequested = true;
        resumeCv.notify_all();
    }

    bool DebugController::isStopped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }

} // namespace cajeta::dbg
