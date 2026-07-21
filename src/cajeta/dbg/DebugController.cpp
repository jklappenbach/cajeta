#include "cajeta/dbg/DebugController.h"

// CP6f-2d: the process-global stop coordinator lives in the C runtime
// (cajeta_runtime.c). The controller drives it: open a stop round before the
// primary blocks so the other carriers quiesce at their safepoints/hand-off,
// and clear it on resume to release them all together.
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
        // stopOnEntry: the first safepoint reached IS the entry method's first
        // executable statement, so an entry stop needs no line resolution.
        // Consumed here under the lock so it fires exactly once.
        const bool entryStop = entryArmed;
        if (entryStop) entryArmed = false;
        const bool breakpointStop = armed.count(locId) != 0;

        // dap-stepping: does this safepoint satisfy the pending step? Only on
        // the origin fiber, only at a different line (multi-statement lines
        // stop once), depth per verb. An armed breakpoint at this safepoint
        // wins over the step (checked first below). No providers -> never.
        bool stepStop = false;
        if (!entryStop && !breakpointStop && stepPending
            && fiberId == stepFiber && depthOfFrame && lineOfLoc
            && lineOfLoc(locId) != stepOriginLine) {
            const int depth = depthOfFrame(frameTop);
            switch (stepKind) {
                case StepKind::In:   stepStop = true; break;
                case StepKind::Over: stepStop = depth <= stepOriginDepth; break;
                case StepKind::Out:  stepStop = depth < stepOriginDepth; break;
            }
        }
        if (!entryStop && !breakpointStop && !stepStop) return;

        // CP6f-2d: open the cross-carrier stop round BEFORE blocking so every
        // other carrier observes it (__cajeta_stop_is_requested) at its next
        // safepoint / scheduler hand-off and parks too. This carrier is the
        // primary; it blocks below via the controller rendezvous as before.
        // §4.3 determinism: if a stop is already in flight (another carrier hit
        // an armed safepoint first this round), request() returns 0 — we are NOT
        // the primary. Don't publish a competing stop; return so the runtime
        // safepoint parks us as an ordinary secondary (__cajeta_stop_park).
        if (__cajeta_stop_request() == 0) {
            // Not the primary this round, so we did NOT take the stop. Put the
            // entry arm back rather than swallowing it.
            if (entryStop) entryArmed = true;
            return;
        }
        // How many carriers must quiesce before inspection: every OTHER carrier
        // in the pool (this primary parks here, in the controller, not the
        // coordinator). <=0 (single carrier / no pool) makes the barrier a
        // no-op. The debugger thread reads this in awaitQuiesce().
        __cajeta_stop_set_expected(__cajeta_carrier_count_get() - 1);

        // Park: publish the stop, wake the debugger thread, wait for resume.
        // Any park consumes the pending step (breakpoint/entry parks clear it;
        // a step park is it completing).
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

    void DebugController::onException(void* throwable, long fiberId,
                                      void* frameTop) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!exceptionArmed) return;

        // CP6f-2d §3.6: an armed exception quiesces the whole pool, exactly like
        // a breakpoint. Open the stop round so other carriers park at their
        // safepoints/hand-off. If a stop is already in flight this round, park as
        // a secondary (the throw resumes after continue) — no competing stop.
        if (__cajeta_stop_request() == 0) {
            lock.unlock();
            __cajeta_stop_park();
            return;
        }
        __cajeta_stop_set_expected(__cajeta_carrier_count_get() - 1);

        // Park with reason=Exception. locId is -1 (no safepoint loc); the DAP
        // layer reads the throwing line from the innermost frame's current_loc.
        stepPending = false;   // an exception park clears any pending step
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
        // CP6f-2d quiesce barrier (spec §2.3). The primary has parked and
        // published expected_count; block until every other carrier parks or
        // the bound elapses, then record how many never quiesced so the DAP
        // layer can flag their fibers. Run WITHOUT the controller mutex (the
        // coordinator has its own leaf lock; carriers parking must not contend
        // on ours), then take the mutex briefly to stamp `current`.
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
        // Clear `stopped` here, under the lock, so a debugger thread that calls
        // waitForStop() right after resume() does NOT re-observe this same stop
        // (the parked carrier clears it too when it wakes, but that happens
        // asynchronously — without clearing here there's a window where the
        // stale stop is seen again, manifesting as a phantom second `stopped`).
        stopped = false;
        resumeRequested = true;
        // A plain continue cancels any pending step (a park already cleared
        // it in the normal flow; this covers a continue issued in between).
        stepPending = false;
        resumeCv.notify_all();
        // CP6f-2d: release every secondary/hand-off-parked carrier together
        // (resume-all, spec §2.5). Clears stop_requested so no carrier re-parks
        // at its next safepoint, and wakes all parked carriers.
        __cajeta_stop_clear();
    }

    void DebugController::resumeWithStep(StepKind kind, long fiberId,
                                         int originDepth, int originLine) {
        std::lock_guard<std::mutex> lock(mutex);
        stepPending = true;
        stepKind = kind;
        stepFiber = fiberId;
        stepOriginDepth = originDepth;
        stepOriginLine = originLine;
        // Same release sequence as resume() (which we can't call here — it
        // would clear the step we just armed).
        stopped = false;
        resumeRequested = true;
        resumeCv.notify_all();
        __cajeta_stop_clear();
    }

    void DebugController::setStepProviders(
        std::function<int(void*)> depthFn, std::function<int(int32_t)> lineFn) {
        std::lock_guard<std::mutex> lock(mutex);
        depthOfFrame = std::move(depthFn);
        lineOfLoc = std::move(lineFn);
    }

    bool DebugController::isStopped() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }

} // namespace cajeta::dbg
