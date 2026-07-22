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
#include <functional>
#include <mutex>
#include <unordered_set>

namespace cajeta::dbg {

    // dap-stepping: the three step verbs. A pending step is armed at resume
    // time (resumeWithStep) and satisfied by the first qualifying safepoint on
    // the origin fiber — see spec 2.1.
    enum class StepKind { In, Over, Out };

    struct StopEvent {
        // Why the program parked. Breakpoint = an armed statement safepoint
        // (CP3); Exception = an armed throw caught at the __cajeta_throw
        // chokepoint (CP6f-3, before the stack unwinds, so the throwing frame
        // is still inspectable).
        // Entry = a one-shot stop at the first safepoint the program reaches,
        // i.e. the entry method's first executable statement (DAP stopOnEntry).
        // Step = a pending step (resumeWithStep) matched this safepoint.
        enum class StopReason { Breakpoint, Exception, Entry, Step };

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
        // CP6f-2d: carriers that did NOT quiesce within the bounded barrier
        // (e.g. blocked in a long native call, unreachable at a safepoint). 0
        // means the whole pool stopped. The DAP layer marks these carriers'
        // fibers running/unavailable rather than presenting stale state.
        int unquiescedCarriers = 0;
    };

    class DebugController {
    public:
        // --- arming (debugger/DAP thread) ---
        // Step-decision trace (live line-132 hunt, 2026-07-22): the last
        // pending-step evaluations, recorded under the existing lock at
        // negligible cost. The DAP layer drains + prints on each step stop
        // so a misbehaving live step explains itself in stderr.
        struct StepDecision {
            int32_t locId;
            long fiberId;
            int depth;
            int originDepth;
            int kind;      // StepKind as int
            bool stopped;
        };
        std::vector<StepDecision> drainStepTrace();

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

        // DAP stopOnEntry: park at the FIRST safepoint reached, whatever its
        // locId, then clear itself. One-shot by construction — a sticky flag
        // would re-park at every later statement, which is a step, not an
        // entry stop. Arm before the program thread starts.
        void armEntry();
        void disarmEntry();
        bool isEntryArmed() const;

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

        // dap-stepping: resume with a pending step. The origin (fiber, frame
        // depth, line) is captured by the caller at the stop; onSafepoint
        // parks with reason Step at the first safepoint on `fiberId` whose
        // line differs from `originLine` and whose depth satisfies the verb
        // (In: any; Over: <= originDepth; Out: < originDepth). Any park —
        // step, breakpoint, exception, entry — clears the pending step, so a
        // step can never wedge the controller.
        // originFrame (9.1): the frame NODE the step was armed on. A
        // candidate safepoint must carry it on its own chain — a foreign
        // carrier's chain never does, so parallel-share safepoints can no
        // longer steal the stop even if their reported fiber id lies.
        void resumeWithStep(StepKind kind, long fiberId, int originDepth,
                            int originLine, void* originFrame = nullptr);

        // Injected seams for step matching: depth of a safepoint's frame-chain
        // head and line of a locId. The controller stays free of frame-walking
        // and loc-table knowledge; the DAP layer wires the real ones. With no
        // providers set, a pending step never matches (safe default).
        void setStepProviders(std::function<int(void*)> depthOfFrame,
                              std::function<int(int32_t)> lineOfLoc,
                              std::function<bool(void*, void*)> containsFrame
                                  = {});

        // Non-blocking: is a safepoint currently parked?
        bool isStopped() const;

        // CP6f-2d: bound on the cross-carrier quiesce barrier. After a safepoint
        // parks the primary, waitForStop blocks until every other carrier parks
        // OR this timeout elapses — so a carrier stuck in a native call can
        // never hang the debugger (spec §2.3, §4.1). Default 500ms.
        void setQuiesceTimeout(std::chrono::milliseconds t);

    private:
        // Run the quiesce barrier (debugger thread): wait until all expected
        // carriers park or quiesceTimeout elapses; record any un-quiesced count
        // on `current`. Caller holds nothing.
        void awaitQuiesce();

        mutable std::mutex mutex;
        std::condition_variable stoppedCv;   // signaled when a safepoint parks
        std::condition_variable resumeCv;    // signaled by resume()
        std::unordered_set<int32_t> armed;
        std::vector<StepDecision> stepTrace;   // ring, capped at 64
        void* stepOriginFrame = nullptr;       // 9.1 chain-identity anchor
        std::function<bool(void*, void*)> containsFrame;
        bool exceptionArmed = false;
        bool entryArmed = false;
        bool stopped = false;
        bool resumeRequested = false;
        StopEvent current;
        std::chrono::milliseconds quiesceTimeout{500};

        // dap-stepping: pending-step state (guarded by `mutex`).
        bool stepPending = false;
        StepKind stepKind = StepKind::In;
        long stepFiber = 0;
        int stepOriginDepth = 0;
        int stepOriginLine = -1;
        std::function<int(void*)> depthOfFrame;
        std::function<int(int32_t)> lineOfLoc;
    };

} // namespace cajeta::dbg
