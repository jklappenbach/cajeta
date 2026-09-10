#include "cajeta/dbg/DebugController.h"

#include "cajeta/error/Diagnostics.h"

#include <sstream>

#include <iostream>

// The process-global stop coordinator lives in the C runtime. This controller
// drives it: open a stop round before the primary blocks so the other carriers
// quiesce at their safepoints, and clear it on resume to release them together.
extern "C" {
    int  __cajeta_stop_request(void);
    void __cajeta_stop_clear(void);
    void __cajeta_stop_park(void);
    void __cajeta_stop_set_expected(int n);
    int  __cajeta_stop_wait_converged(long timeout_ns);
    int  __cajeta_carrier_count_get(void);
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

    void DebugController::armEntry() {
        std::lock_guard<std::mutex> lock(mutex);
        entryArmed = true;
    }

    void DebugController::disarmEntry() {
        std::lock_guard<std::mutex> lock(mutex);
        entryArmed = false;
    }

    bool DebugController::isEntryArmed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return entryArmed;
    }

    void DebugController::onSafepoint(int32_t locId, long fiberId) {
        onSafepoint(locId, fiberId, nullptr);
    }

    void DebugController::onSafepoint(int32_t locId, long fiberId,
                                     void* frameTop) {
        std::unique_lock<std::mutex> lock(mutex);
        // The entry arm is consumed here, under the lock, so it fires once.
        const bool entryStop = entryArmed;
        if (entryStop) entryArmed = false;
        const bool breakpointStop = armed.count(locId) != 0;

        bool stepStop = false;
        if (!entryStop && !breakpointStop && stepPending && depthOfFrame
            && lineOfLoc) {
            const int depth = depthOfFrame(frameTop);
            int reason = 0;
            if (fiberId != stepFiber) reason = 1;
            else if (lineOfLoc(locId) == stepOriginLine) reason = 2;
            // Chain identity keeps a foreign chain from stealing the stop, but
            // only at or below the origin depth: a step that returns PAST the
            // origin leaves that frame behind and would never be satisfiable.
            else if (containsFrame && stepOriginFrame
                     && depth >= stepOriginDepth
                     && !containsFrame(frameTop, stepOriginFrame)) reason = 3;
            if (reason == 0) {
                switch (stepKind) {
                    case StepKind::In:   stepStop = true; break;
                    case StepKind::Over: stepStop = depth <= stepOriginDepth; break;
                    case StepKind::Out:  stepStop = depth < stepOriginDepth; break;
                }
            }
            if (stepTrace.size() >= 64) stepTrace.erase(stepTrace.begin());
            stepTrace.push_back(StepDecision{locId, fiberId, depth,
                                             stepOriginDepth, (int) stepKind,
                                             stepStop, reason});
        }
        if (!entryStop && !breakpointStop && !stepStop) return;

        // Open the cross-carrier round BEFORE blocking, so every other carrier
        // parks at its next safepoint. A 0 means another carrier is already the
        // primary this round: take no stop here and park as an ordinary secondary.
        if (__cajeta_stop_request() == 0) {
            if (entryStop) entryArmed = true;
            return;
        }
        // Every OTHER carrier must quiesce; this primary parks in the controller,
        // not the coordinator, and <=0 makes awaitQuiesce's barrier a no-op.
        __cajeta_stop_set_expected(__cajeta_carrier_count_get() - 1);

        stepPending = false;
        stopped = true;
        resumeRequested = false;
        current = StopEvent{locId, fiberId, frameTop};
        current.reason = entryStop        ? StopEvent::StopReason::Entry
                         : breakpointStop ? StopEvent::StopReason::Breakpoint
                                          : StopEvent::StopReason::Step;
        stoppedCv.notify_all();
        resumeCv.wait(lock, [this] { return resumeRequested; });
        stopped = false;
    }

    std::vector<DebugController::StepDecision> DebugController::drainStepTrace() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<StepDecision> out;
        out.swap(stepTrace);
        return out;
    }

    void DebugController::onException(void* throwable, long fiberId,
                                      void* frameTop) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!exceptionArmed) return;

        // An armed exception quiesces the pool exactly like a breakpoint; when a
        // round is already in flight this thread parks as a plain secondary.
        if (__cajeta_stop_request() == 0) {
            lock.unlock();
            __cajeta_stop_park();
            return;
        }
        __cajeta_stop_set_expected(__cajeta_carrier_count_get() - 1);

        // locId -1: the DAP layer reads the throwing line from the frame instead.
        stepPending = false;
        stopped = true;
        resumeRequested = false;
        current = StopEvent{-1, fiberId, frameTop,
                            StopEvent::StopReason::Exception, throwable};
        stoppedCv.notify_all();
        resumeCv.wait(lock, [this] { return resumeRequested; });
        stopped = false;
    }

    void DebugController::setQuiesceTimeout(std::chrono::milliseconds t) {
        std::lock_guard<std::mutex> lock(mutex);
        quiesceTimeout = t;
    }

    void DebugController::awaitQuiesce() {
        // Blocks until every other carrier parks or the bound elapses, recording
        // the stragglers. Runs WITHOUT the controller mutex — parking carriers
        // must not contend on it — and takes it only to stamp `current`.
        long timeoutNs =
            static_cast<long>(quiesceTimeout.count()) * 1000L * 1000L;
        int unquiesced = __cajeta_stop_wait_converged(timeoutNs);
        std::lock_guard<std::mutex> lock(mutex);
        current.unquiescedCarriers = unquiesced;
    }

    StopEvent DebugController::waitForStop() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stoppedCv.wait(lock, [this] { return stopped; });
        }
        awaitQuiesce();   // barrier: don't report until the pool has quiesced
        std::lock_guard<std::mutex> lock(mutex);
        return current;
    }

    bool DebugController::waitForStop(StopEvent& out,
                                      std::chrono::milliseconds timeout) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!stoppedCv.wait_for(lock, timeout, [this] { return stopped; })) {
                return false;
            }
        }
        awaitQuiesce();   // barrier before the DAP `stopped` is emitted
        std::lock_guard<std::mutex> lock(mutex);
        out = current;
        return true;
    }

    void DebugController::resume() {
        std::lock_guard<std::mutex> lock(mutex);
        // Clear `stopped` here, not only in the waking carrier: that clear is
        // asynchronous, and the window would show as a phantom second stop.
        stopped = false;
        resumeRequested = true;
        stepPending = false;
        resumeCv.notify_all();
        // Resume-all: clears stop_requested so no carrier re-parks, and wakes
        // every parked one.
        __cajeta_stop_clear();
    }

    void DebugController::resumeWithStep(StepKind kind, long fiberId,
                                         int originDepth, int originLine,
                                         void* originFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        stepPending = true;
        stepKind = kind;
        stepFiber = fiberId;
        stepOriginDepth = originDepth;
        stepOriginLine = originLine;
        stepOriginFrame = originFrame;
        {
            std::ostringstream armed;
            armed << "[step-armed] kind=" << (int) kind << " fiber=" << fiberId
                  << " originDepth=" << originDepth
                  << " originFrame=" << originFrame << "\n";
            cajeta::logLine("debug", armed.str());
        }
        // resume()'s release sequence, inlined: calling it would clear the step.
        stopped = false;
        resumeRequested = true;
        resumeCv.notify_all();
        __cajeta_stop_clear();
    }

    void DebugController::setStepProviders(
        std::function<int(void*)> depthFn, std::function<int(int32_t)> lineFn,
        std::function<bool(void*, void*)> containsFn) {
        std::lock_guard<std::mutex> lock(mutex);
        depthOfFrame = std::move(depthFn);
        lineOfLoc = std::move(lineFn);
        containsFrame = std::move(containsFn);
    }

    bool DebugController::isStopped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }

} // namespace cajeta::dbg
