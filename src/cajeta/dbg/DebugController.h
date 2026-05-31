//
// In-process debug controller (debugger CP3).
//
// Owns the breakpoint "armed" set and the stop/resume rendezvous between the
// executing thread (a fiber carrier, which hits __cajeta_dbg_safepoint) and a
// separate debugger thread (the test harness now; the `cajeta dap` server
// later). The model is stop-the-world, one breakpoint at a time:
//
//   carrier thread:  onSafepoint(locId, fiberId)
//        - not armed -> returns immediately
//        - armed     -> records the stop, signals, and BLOCKS until resume()
//
//   debugger thread: waitForStop() -> StopEvent ; ... ; resume()
//
// This is the seam CP4's DAP server drives: setBreakpoints -> arm/disarm,
// `stopped` event <- waitForStop, continue -> resume.
//
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace cajeta::dbg {

    struct StopEvent {
        int32_t locId = -1;
        long fiberId = 0;
    };

    class DebugController {
    public:
        // --- arming (debugger/DAP thread) ---
        void arm(int32_t locId);
        void disarm(int32_t locId);
        void clearArmed();
        bool isArmed(int32_t locId) const;

        // --- executing (carrier) thread ---
        // At a statement safepoint. If locId is armed, record the stop, wake
        // any waitForStop(), and block until resume(). No-op if not armed.
        void onSafepoint(int32_t locId, long fiberId);

        // --- debugger thread ---
        // Block until a safepoint parks, then return what stopped.
        StopEvent waitForStop();
        // Bounded wait: fill `out` and return true if a safepoint parks within
        // `timeout`, else return false. Lets callers (and tests) avoid hanging
        // when nothing ever stops.
        bool waitForStop(StopEvent& out, std::chrono::milliseconds timeout);
        // Release the parked safepoint so the carrier thread continues.
        void resume();

        // Non-blocking: is a safepoint currently parked?
        bool isStopped() const;

    private:
        mutable std::mutex mutex;
        std::condition_variable stoppedCv;   // signaled when a safepoint parks
        std::condition_variable resumeCv;    // signaled by resume()
        std::unordered_set<int32_t> armed;
        bool stopped = false;
        bool resumeRequested = false;
        StopEvent current;
    };

} // namespace cajeta::dbg
