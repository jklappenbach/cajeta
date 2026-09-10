// In-process debug controller: owns the armed-breakpoint set and the stop/resume
// rendezvous between a carrier thread (which hits __cajeta_dbg_safepoint) and the
// debugger thread. Stop-the-world, one breakpoint at a time.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>

namespace cajeta::dbg {

    // The three step verbs; a pending step is armed by resumeWithStep().
    enum class StepKind { In, Over, Out };

    struct StopEvent {
        // Exception parks before the stack unwinds; Entry is the one-shot stop at
        // the first safepoint reached; Step means a pending step matched here.
        enum class StopReason { Breakpoint, Exception, Entry, Step };

        int32_t locId = -1;
        long fiberId = 0;
        // Opaque here, and valid only while the carrier is parked.
        void* frameTop = nullptr;
        // locId is -1 for an exception stop; use the frame's current_loc.
        StopReason reason = StopReason::Breakpoint;
        void* throwable = nullptr;
        // Carriers that missed the bounded barrier; 0 means the whole pool parked.
        int unquiescedCarriers = 0;
    };

    class DebugController {
    public:
        // --- arming (debugger/DAP thread) ---
        // The last pending-step evaluations, drained by the DAP layer per stop.
        struct StepDecision {
            int32_t locId;
            long fiberId;
            int depth;
            int originDepth;
            int kind;      // StepKind as int
            bool stopped;
            int reason;    // 0=evaluated 1=fiber 2=same-line 3=chain-mismatch
        };
        std::vector<StepDecision> drainStepTrace();

        void arm(int32_t locId);
        void disarm(int32_t locId);
        void clearArmed();
        bool isArmed(int32_t locId) const;

        // Break on thrown exceptions — a single all-throws toggle, no type filter.
        void armException();
        void disarmException();
        bool isExceptionArmed() const;

        // One-shot park at the FIRST safepoint reached, whatever its locId.
        void armEntry();
        void disarmEntry();
        bool isEntryArmed() const;

        // --- executing (carrier) thread ---
        // When `locId` is armed: record the stop, wake waitForStop(), block.
        void onSafepoint(int32_t locId, long fiberId);
        // Overload carrying the frame-chain head; the 2-arg form passes nullptr.
        void onSafepoint(int32_t locId, long fiberId, void* frameTop);

        // At a throw, pre-unwind: parks when exceptions are armed, else a no-op.
        void onException(void* throwable, long fiberId, void* frameTop);

        // --- debugger thread ---
        // Blocks until a safepoint parks, then returns what stopped.
        StopEvent waitForStop();
        // Bounded wait: fills `out` and returns true when one parks in `timeout`.
        bool waitForStop(StopEvent& out, std::chrono::milliseconds timeout);
        // Resume-all with no pending step: clears the stop here rather than leaving
        // it to the waking carrier (that clear is asynchronous and the window reads
        // as a phantom second stop), then wakes every parked carrier.
        void resume();

        // Resumes with a pending step: parks at the first safepoint on `fiberId`
        // past `originLine` whose depth suits the verb (In any, Over <=, Out <),
        // and only on a chain carrying `originFrame`. Any park clears the step.
        void resumeWithStep(StepKind kind, long fiberId, int originDepth,
                            int originLine, void* originFrame = nullptr);

        // Injected step-matching seams; with none set, a pending step never matches.
        void setStepProviders(std::function<int(void*)> depthOfFrame,
                              std::function<int(int32_t)> lineOfLoc,
                              std::function<bool(void*, void*)> containsFrame
                                  = {});

        bool isStopped() const;

        // Bounds the cross-carrier quiesce barrier, so a carrier stuck in a native
        // call can never hang the debugger. Default 500ms.
        void setQuiesceTimeout(std::chrono::milliseconds t);

    private:
        // Runs the quiesce barrier, recording un-quiesced carriers on `current`.
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

        // Pending-step state, guarded by `mutex`.
        bool stepPending = false;
        StepKind stepKind = StepKind::In;
        long stepFiber = 0;
        int stepOriginDepth = 0;
        int stepOriginLine = -1;
        std::function<int(void*)> depthOfFrame;
        std::function<int(int32_t)> lineOfLoc;
    };

} // namespace cajeta::dbg
