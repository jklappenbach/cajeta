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
        // Why the program parked. Breakpoint = an armed statement safepoint
        // (CP3); Exception = an armed throw caught at the __cajeta_throw
        // chokepoint (CP6f-3, before the stack unwinds, so the throwing frame
        // is still inspectable).
        enum class StopReason { Breakpoint, Exception };

        int32_t locId = -1;
        long fiberId = 0;
        // CP5: the dbg frame-chain head captured at the safepoint. Opaque to
        // the controller; DebugVars::walkFrames dereferences it (via the
        // runtime's stateless accessors) to produce frames + locals. Valid
        // only while the carrier is parked (until the matching resume()).
        void* frameTop = nullptr;
        // CP6f-3: why we stopped, and (for Exception) the thrown Throwable*
        // (opaque here; the DAP layer may render it). locId is -1 for an
        // exception stop — there's no safepoint loc; the throwing line comes
        // from the innermost frame's recorded current_loc.
        StopReason reason = StopReason::Breakpoint;
        void* throwable = nullptr;
    };

    class DebugController {
    public:
        // --- arming (debugger/DAP thread) ---
        void arm(int32_t locId);
        void disarm(int32_t locId);
        void clearArmed();
        bool isArmed(int32_t locId) const;

        // CP6f-3: break on thrown exceptions. When armed, the next
        // onException() (driven from the runtime throw chokepoint) parks like a
        // breakpoint. A single all-throws toggle for now (no type filter yet).
        void armException();
        void disarmException();
        bool isExceptionArmed() const;

        // --- executing (carrier) thread ---
        // At a statement safepoint. If locId is armed, record the stop, wake
        // any waitForStop(), and block until resume(). No-op if not armed.
        void onSafepoint(int32_t locId, long fiberId);
        // CP5 overload carrying the dbg frame-chain head (for locals/scopes).
        // The 2-arg form delegates here with frameTop=nullptr.
        void onSafepoint(int32_t locId, long fiberId, void* frameTop);

        // CP6f-3: at a throw (the runtime's __cajeta_throw chokepoint, before
        // the stack unwinds). If exceptions are armed, record the stop with
        // reason=Exception + the thrown value and BLOCK until resume(); else
        // no-op so the throw proceeds normally.
        void onException(void* throwable, long fiberId, void* frameTop);

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
        bool exceptionArmed = false;
        bool stopped = false;
        bool resumeRequested = false;
        StopEvent current;
    };

} // namespace cajeta::dbg
