//
// SharedPoolHarness.h — NET-4.3 Model-B (event-driven shared-pool) harness.
//
// The NET-4.3 shared-pool server (runtime/src/cajeta/io/net/SharedPoolServer.cajeta)
// is, like the NET-4.1 core it extends, a piece of *pure dispatch logic*
// riding on primitives that already exist (the NET-4.1 Server lifecycle +
// in-flight drain, cajeta.concurrent.Channel, AtomicInt32). The Model-B
// override changes exactly two things vs. Model A (fiber-per-connection):
//
//   1. dispatch() ENQUEUES the accepted connection onto a *bounded*
//      Channel<TcpStream> instead of spawning one fiber per connection. A
//      full queue parks the accept-loop fiber (Channel.send back-pressure),
//      so connections wait in the kernel accept backlog rather than
//      admitting unbounded user-space fibers.
//
//   2. serve() spawns N worker fibers that each drain the queue and run a
//      handler turn per dequeued connection, until the queue is closed AND
//      drained. shutdown() closes the queue so the workers exit, then reuses
//      the inherited deadline-bounded in-flight drain.
//
// The property that distinguishes Model B from Model A — and the one the
// `ServerTests.sharedPoolBoundsConcurrentFibers` acceptance pins — is that
// the peak number of fibers *running a handler* is bounded by the pool size
// no matter how many clients connect.
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Identical rationale to ServerLifecycleHarness.h (NET-4.1) and the other
// net harnesses (LoopbackFixtures.h, ReactorHarness.h): the cajeta-surface
// socket lowering the full SharedPoolServer.serve() accept loop needs
// (NET-1.3/1.4 "partial: needs compiler-core net dispatch") is still being
// wired, so the *full* loop cannot be JIT-run deterministically yet. But the
// bounded-pool dispatch LOGIC the override encodes — a bounded work queue
// drained by N workers, with a peak-concurrency bound, full drain, and a
// queue-close-driven graceful stop — is platform-independent and
// deterministically pinnable on its own. That is exactly what this header
// models, one level down, with a std::mutex + condition_variable bounded
// queue standing in for cajeta.concurrent.Channel<TcpStream>, std::atomic for
// AtomicInt32, and std::thread for the worker fibers.
//
// The queue/worker/drain semantics here MUST mirror
// runtime/src/cajeta/io/net/SharedPoolServer.cajeta exactly — they are the
// executable spec for that pure-logic override. Kept under test/ so the
// production sources carry no test-only surface.
//
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // BoundedQueue<T> — a faithful native model of the bounded
    // cajeta.concurrent.Channel<T> the shared pool drains: a fixed-capacity
    // FIFO guarded by a mutex + condvar. send() blocks while full (the
    // accept-loop back-pressure); receive() blocks while empty and open, and
    // returns "no value" once the queue is closed AND drained (mirroring
    // Channel.receive's empty Optional). close() is one-way + idempotent and
    // wakes every blocked sender/receiver — exactly Channel.close().
    // -----------------------------------------------------------------------
    template <typename T>
    class BoundedQueue {
    public:
        explicit BoundedQueue(int32_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {}

        // Enqueue, blocking while full and open. No-op-throws are out of
        // scope here (the Cajeta send raises on a closed channel; the pool
        // never sends post-close, so the model simply drops a post-close
        // send to keep the harness total).
        void send(T item) {
            std::unique_lock<std::mutex> lk(m_);
            notFull_.wait(lk, [&] { return size_ < capacity_ || closed_; });
            if (closed_) return;
            buf_.push_back(item);
            ++size_;
            notEmpty_.notify_one();
        }

        // Dequeue the next item. Returns true with `out` set while items
        // remain (draining buffered items even after close); returns false
        // once the queue is closed AND drained — the worker's exit signal.
        bool receive(T& out) {
            std::unique_lock<std::mutex> lk(m_);
            notEmpty_.wait(lk, [&] { return size_ > 0 || closed_; });
            if (size_ == 0) return false;   // closed and drained
            out = buf_.front();
            buf_.pop_front();
            --size_;
            notFull_.notify_one();
            return true;
        }

        // One-way close. Wakes every blocked sender/receiver to re-check.
        void close() {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
            notEmpty_.notify_all();
            notFull_.notify_all();
        }

        int32_t size() {
            std::lock_guard<std::mutex> lk(m_);
            return size_;
        }

    private:
        const int32_t          capacity_;
        std::deque<T>          buf_;
        int32_t                size_ = 0;
        bool                   closed_ = false;
        std::mutex             m_;
        std::condition_variable notEmpty_;
        std::condition_variable notFull_;
    };

    // -----------------------------------------------------------------------
    // SharedPool — a faithful native model of SharedPoolServer's Model-B
    // dispatch: a bounded work queue drained by N worker threads (standing in
    // for the worker fibers), with the same in-flight + queued accounting and
    // the same queue-close-driven graceful stop.
    //
    //   * dispatch(conn)  ≡ SharedPoolServer.dispatch(): bump inflight + queued,
    //                       then enqueue (parks on a full queue — back-pressure).
    //   * worker turn      ≡ SharedPoolServer.worker()+runTurn(): receive, drop
    //                       queued, run the handler, drop inflight in a finally.
    //   * start(n)         ≡ serve()'s `spawn worker` ×N.
    //   * shutdown()       ≡ SharedPoolServer.shutdown(): close the queue so the
    //                       workers drain remaining + exit, then join.
    //
    // The handler is a caller-supplied std::function<void(int)> run per
    // dequeued connection id. The harness pins:
    //   - peak concurrent handler turns ≤ pool size (the Model-B bound),
    //   - every dispatched connection handled exactly once (full drain),
    //   - inflight + queued both return to zero after a clean stop,
    //   - a throwing handler turn still drops inflight and the worker survives.
    // -----------------------------------------------------------------------
    template <typename Handler>
    class SharedPool {
    public:
        // capacity defaults to poolSize — the SharedPoolServer default: the
        // accept loop may stage one pending connection per worker.
        SharedPool(int32_t poolSize, Handler handler)
            : poolSize_(poolSize < 1 ? 1 : poolSize),
              queue_(poolSize < 1 ? 1 : poolSize),
              handler_(handler) {}

        SharedPool(int32_t poolSize, int32_t capacity, Handler handler)
            : poolSize_(poolSize < 1 ? 1 : poolSize),
              queue_(capacity),
              handler_(handler) {}

        // serve()'s `spawn worker` ×poolSize.
        void start() {
            for (int32_t i = 0; i < poolSize_; ++i) {
                workers_.emplace_back([this] { this->workerLoop(); });
            }
        }

        // dispatch(): bump inflight + queued at admit time, then enqueue
        // (parking on a full queue). The connection "id" stands in for the
        // accepted TcpStream.
        void dispatch(int connId) {
            inflight_.fetch_add(1, std::memory_order_seq_cst);
            queued_.fetch_add(1, std::memory_order_seq_cst);
            queue_.send(connId);
        }

        // shutdown(): close the queue so the workers drain remaining + exit,
        // then join them — the structured-concurrency join the serve() scope
        // performs. Returns after every worker has exited.
        void shutdown() {
            queue_.close();
            for (auto& t : workers_) {
                if (t.joinable()) t.join();
            }
        }

        int32_t workerCount() const { return poolSize_; }
        int32_t inflight()    { return inflight_.load(std::memory_order_seq_cst); }
        int32_t queued()      { return queued_.load(std::memory_order_seq_cst); }

        // The peak number of handler turns observed running concurrently —
        // the Model-B bound the sharedPoolBoundsConcurrentFibers acceptance
        // asserts stays ≤ poolSize.
        int32_t peakConcurrentHandlers() const {
            return peakActive_.load(std::memory_order_seq_cst);
        }

        // Total handler turns that ran — must equal the dispatched count (no
        // connection dropped, none handled twice).
        int32_t handledCount() const {
            return handled_.load(std::memory_order_seq_cst);
        }

    private:
        // worker()+runTurn(): drain the queue, drop `queued` on dequeue, run
        // the handler, drop `inflight` in a finally (so a throwing handler
        // still decrements and the worker loops on — never tearing the pool
        // down).
        void workerLoop() {
            int connId = 0;
            while (queue_.receive(connId)) {
                queued_.fetch_sub(1, std::memory_order_seq_cst);   // dequeued: now in-handler

                // Track peak concurrency: bump active, publish the max, run.
                const int32_t active = active_.fetch_add(1, std::memory_order_seq_cst) + 1;
                bumpPeak(active);

                try {
                    handler_(connId);
                    handled_.fetch_add(1, std::memory_order_seq_cst);
                } catch (...) {
                    // Isolated to this turn (still counts as handled — the
                    // connection was processed, the handler just threw) and
                    // the `finally` decrements run below; the worker survives.
                    handled_.fetch_add(1, std::memory_order_seq_cst);
                }

                // The `finally`: drop active, then drop inflight.
                active_.fetch_sub(1, std::memory_order_seq_cst);
                inflight_.fetch_sub(1, std::memory_order_seq_cst);
            }
        }

        void bumpPeak(int32_t observed) {
            int32_t cur = peakActive_.load(std::memory_order_seq_cst);
            while (observed > cur &&
                   !peakActive_.compare_exchange_weak(cur, observed,
                                                      std::memory_order_seq_cst)) {
                // cur reloaded by compare_exchange_weak; retry.
            }
        }

        const int32_t            poolSize_;
        BoundedQueue<int>        queue_;
        Handler                  handler_;
        std::vector<std::thread> workers_;
        std::atomic<int32_t>     inflight_{0};
        std::atomic<int32_t>     queued_{0};
        std::atomic<int32_t>     active_{0};
        std::atomic<int32_t>     peakActive_{0};
        std::atomic<int32_t>     handled_{0};
    };

} // namespace cajeta::net::testing
