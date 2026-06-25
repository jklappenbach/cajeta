#include "cajeta/dbg/DebugController.h"

// CP6f-2d: the process-global stop coordinator lives in the C runtime
// (cajeta_runtime.c). The controller drives it: open a stop round before the
// primary blocks so the other carriers quiesce at their safepoints/hand-off,
// and clear it on resume to release them all together.
extern "C" {
    int  __cajeta_stop_request(void);
    void __cajeta_stop_clear(void);
}

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

    void DebugController::armException() {
        std::lock_guard<std::mutex> lock(mutex);
        exceptionArmed = true;
    }

    void DebugController::disarmException() {
        std::lock_guard<std::mutex> lock(mutex);
        exceptionArmed = false;
    }

    bool DebugController::isExceptionArmed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return exceptionArmed;
    }

    void DebugController::onSafepoint(int32_t locId, long fiberId) {
        onSafepoint(locId, fiberId, nullptr);
    }

    void DebugController::onSafepoint(int32_t locId, long fiberId,
                                     void* frameTop) {
        std::unique_lock<std::mutex> lock(mutex);
        if (armed.count(locId) == 0) return;

        // CP6f-2d: open the cross-carrier stop round BEFORE blocking so every
        // other carrier observes it (__cajeta_stop_is_requested) at its next
        // safepoint / scheduler hand-off and parks too. This carrier is the
        // primary; it blocks below via the controller rendezvous as before.
        __cajeta_stop_request();

        // Park: publish the stop, wake the debugger thread, wait for resume.
        stopped = true;
        resumeRequested = false;
        current = StopEvent{locId, fiberId, frameTop};
        stoppedCv.notify_all();
        resumeCv.wait(lock, [this] { return resumeRequested; });
        stopped = false;
    }

    void DebugController::onException(void* throwable, long fiberId,
                                      void* frameTop) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!exceptionArmed) return;

        // Park with reason=Exception. locId is -1 (no safepoint loc); the DAP
        // layer reads the throwing line from the innermost frame's current_loc.
        stopped = true;
        resumeRequested = false;
        current = StopEvent{-1, fiberId, frameTop,
                            StopEvent::StopReason::Exception, throwable};
        stoppedCv.notify_all();
        resumeCv.wait(lock, [this] { return resumeRequested; });
        stopped = false;
    }

    StopEvent DebugController::waitForStop() {
        std::unique_lock<std::mutex> lock(mutex);
        stoppedCv.wait(lock, [this] { return stopped; });
        return current;
    }

    bool DebugController::waitForStop(StopEvent& out,
                                      std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!stoppedCv.wait_for(lock, timeout, [this] { return stopped; })) {
            return false;
        }
        out = current;
        return true;
    }

    void DebugController::resume() {
        std::lock_guard<std::mutex> lock(mutex);
        // Clear `stopped` here, under the lock, so a debugger thread that calls
        // waitForStop() right after resume() does NOT re-observe this same stop
        // (the parked carrier clears it too when it wakes, but that happens
        // asynchronously — without clearing here there's a window where the
        // stale stop is seen again, manifesting as a phantom second `stopped`).
        stopped = false;
        resumeRequested = true;
        resumeCv.notify_all();
        // CP6f-2d: release every secondary/hand-off-parked carrier together
        // (resume-all, spec §2.5). Clears stop_requested so no carrier re-parks
        // at its next safepoint, and wakes all parked carriers.
        __cajeta_stop_clear();
    }

    bool DebugController::isStopped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }

} // namespace cajeta::dbg
