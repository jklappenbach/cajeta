// C++ concurrency competitors for bench/concurrent/*.md — every id, the same
// workload, size and check as the Cajeta side (bench/src/main/cajeta/bench/concurrent/).
// One v1 row per id (lib/rows.sh lifts it to v2). The contended rows use OS
// threads (std::thread) where Cajeta uses fibers. Under CAJETA_BENCH_SMOKE=1 the
// multi-thread rows run 1e4 operations per thread (channels: 1e4 messages) and
// the 1e8 single-thread rows 1e6; those rows say "smoke size" in notes.
// Ids the page does not list a C++ competitor for are emitted as skipped rows
// with the reason. Results are consumed (returned and checked) so nothing is
// elided; the thread_local row adds a compiler fence per pass so the store and
// the load both happen.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <numeric>
#include <cerrno>
#include <semaphore.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#include <new>

#include "blockingconcurrentqueue.h"

using clk = std::chrono::steady_clock;
typedef long long i64;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return (v && *v) ? v : d; }
static bool SMOKE = false;

static std::atomic<unsigned long long> _ALLOCED{0};
void* operator new(std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static i64 peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        i64 kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static std::string RUN_ID, TS;
static int WARMUP = 3, TRIALS = 10;

static const char* LIBVER = "std";
static void emit_row(const char* bench, const char* lib, i64 input, int warmup, int trials,
                     std::vector<i64> s, bool ok, unsigned long long alloc, const std::string& notes) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    i64 sum = std::accumulate(s.begin(), s.end(), 0LL);
    i64 mn = s.front(), med = s[n / 2], mean = sum / (i64)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,concurrent,,%lld,,%lld,cpp,%s,%s,%s,-O3 -march=native -pthread,%d,%d,"
        "%lld,%lld,%lld,%lld,%.3f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,%s,\n",
        RUN_ID.c_str(), TS.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, LIBVER, warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), alloc,
        ok ? "ok" : "invalid", ok ? "true" : "false", notes.c_str());
    std::fflush(stdout);
}

static void skip_row(const char* bench, i64 input, const char* reason, const char* lib = "") {
    std::printf("1,%s,%s,%s,concurrent,,%lld,,%lld,cpp,%s,%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,\n",
        RUN_ID.c_str(), TS.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(), lib, reason);
    std::fflush(stdout);
}

static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "; " + b;
}
static std::string smoke_note(const char* what) { return SMOKE ? std::string("smoke size: ") + what : std::string(); }

// Run f warmup+trials times, check every trial, one untimed run for allocation.
static void bench(const char* name, const char* lib, i64 input, const std::string& notes,
                  const std::function<i64()>& f, const std::function<bool(i64)>& check) {
    // PROFILE_ONLY=<substring> runs only the matching ids (a debugging aid).
    std::string only = env("PROFILE_ONLY", "");
    if (!only.empty() && std::string(name).find(only) == std::string::npos) return;
    for (int i = 0; i < WARMUP; ++i) { volatile i64 r = f(); (void)r; }
    std::vector<i64> s; bool ok = true;
    for (int i = 0; i < TRIALS; ++i) {
        auto t0 = clk::now(); i64 r = f(); auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = ok && check(r);
    }
    _ALLOCED = 0; { volatile i64 r = f(); (void)r; } unsigned long long al = _ALLOCED.load();
    emit_row(name, lib, input, WARMUP, TRIALS, s, ok, al, notes);
}

template <class F> static void run_threads(int t, F body) {
    std::vector<std::thread> th; th.reserve(t);
    for (int k = 0; k < t; ++k) th.emplace_back([&body, k]{ body(k); });
    for (auto& x : th) x.join();
}

thread_local i64 TL_SLOT = 0;

int main() {
    RUN_ID = env("PROFILE_RUN_ID", "local");
    TS = env("PROFILE_RUN_TS", "");
    WARMUP = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    TRIALS = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    std::string sm = env("CAJETA_BENCH_SMOKE", "");
    SMOKE = (sm == "1" || sm == "true");

    const i64 N1 = SMOKE ? 1000000 : 100000000;          // the 1e8 single-thread rows
    const i64 PER = SMOKE ? 10000 : 1000000;             // per-thread ops, contended rows
    const i64 NCH = SMOKE ? 10000 : 5000000;             // channel messages
    const i64 NSP = SMOKE ? 10000 : 100000;              // spawn+await
    const std::string s1 = smoke_note("1e6 ops"), sc = smoke_note("1e4 ops per thread");

    bench("atomic-fetchadd", "std::atomic<int64_t>", N1, join("seq_cst fetch_add", s1), [&]{
        std::atomic<i64> a{0};
        for (i64 i = 0; i < N1; ++i) a.fetch_add(1, std::memory_order_seq_cst);
        return a.load();
    }, [&](i64 r){ return r == N1; });

    bench("atomic-fetchadd-32", "std::atomic<int32_t>", N1, join("seq_cst fetch_add", s1), [&]{
        std::atomic<int32_t> a{0};
        for (i64 i = 0; i < N1; ++i) a.fetch_add(1, std::memory_order_seq_cst);
        return (i64)a.load();
    }, [&](i64 r){ return r == N1; });

    bench("atomic-cas", "std::atomic<int64_t>", N1, join("compare_exchange_strong seq_cst; uncontended", s1), [&]{
        std::atomic<i64> a{0}; i64 fails = 0;
        for (i64 i = 0; i < N1; ++i) { i64 e = i; if (!a.compare_exchange_strong(e, i + 1)) ++fails; }
        return fails == 0 ? a.load() : -1;
    }, [&](i64 r){ return r == N1; });

    bench("atomic-contended-16", "std::atomic+std::thread", 16 * PER, join("16 OS threads", sc), [&]{
        std::atomic<i64> a{0};
        run_threads(16, [&](int){ for (i64 i = 0; i < PER; ++i) a.fetch_add(1); });
        return a.load();
    }, [&](i64 r){ return r == 16 * PER; });

    bench("mutex-uncontended", "std::mutex", N1, join("lock; ++v; unlock on one thread", s1), [&]{
        std::mutex m; i64 v = 0;
        for (i64 i = 0; i < N1; ++i) { std::lock_guard<std::mutex> g(m); ++v; }
        return v;
    }, [&](i64 r){ return r == N1; });

    for (int T : {4, 16}) {
        std::string id = "mutex-contended-" + std::to_string(T);
        bench(id.c_str(), "std::mutex+std::thread", T * PER, join(std::to_string(T) + " OS threads", sc), [&]{
            std::mutex m; i64 v = 0;
            run_threads(T, [&](int){ for (i64 i = 0; i < PER; ++i) { std::lock_guard<std::mutex> g(m); ++v; } });
            return v;
        }, [&](i64 r){ return r == T * PER; });
    }

    bench("rwlock-read-heavy", "std::shared_mutex", 16 * PER, join("16 OS threads 90/10 read/write", sc), [&]{
        std::shared_mutex m; i64 v = 0; std::atomic<i64> reads{0};
        run_threads(16, [&](int){
            i64 s = 0;
            for (i64 i = 0; i < PER; ++i) {
                if (i % 10 == 0) { std::unique_lock<std::shared_mutex> g(m); ++v; }
                else { std::shared_lock<std::shared_mutex> g(m); s += v; }
            }
            reads += s;
        });
        return reads.load() >= 0 ? v : -1;
    }, [&](i64 r){ return r == 16 * (PER / 10); });

    bench("lock-uncontended", "std::mutex", N1, join("std::lock_guard per pass on one thread", s1), [&]{
        std::mutex m; i64 c = 0;
        for (i64 i = 0; i < N1; ++i) { std::lock_guard<std::mutex> g(m); ++c; }
        return c;
    }, [&](i64 r){ return r == N1; });

    // std::counting_semaphore (libstdc++ 13.3, g++ and the fork's clang alike)
    // loses wake-ups at 16 threads x 4 permits: a 16 x 1e4 acquire/release loop
    // hangs within a few rounds (measured 2026-10-25). The page's row is kept as
    // a skipped row naming that; POSIX sem_t, the closest primitive, is measured.
    skip_row("semaphore-acquire-release", 16 * PER,
             "std::counting_semaphore in libstdc++ 13.3 hangs (lost wake-up) at 16 threads x 4 permits; measured with POSIX sem_t in the next row", "std::counting_semaphore");
    bench("semaphore-acquire-release", "sem_t (POSIX)", 16 * PER,
          join("16 OS threads through 4 permits; stands in for std::counting_semaphore (libstdc++ 13.3 hangs)", smoke_note("1e4 passes per thread")), [&]{
        sem_t sem; sem_init(&sem, 0, 4);
        std::atomic<i64> inside{0}, passes{0}, viol{0}, errs{0};
        run_threads(16, [&](int){
            for (i64 i = 0; i < PER; ++i) {
                while (sem_wait(&sem) != 0) { if (errno != EINTR) { errs.fetch_add(1); break; } }
                if (inside.fetch_add(1) + 1 > 4) viol.fetch_add(1);
                inside.fetch_sub(1);
                sem_post(&sem);
            }
            passes.fetch_add(PER);
        });
        sem_destroy(&sem);
        return (viol.load() == 0 && errs.load() == 0) ? passes.load() : -1;
    }, [&](i64 r){ return r == 16 * PER; });

    // Channels: moodycamel::BlockingConcurrentQueue sized for P explicit
    // producers, bounded at 1e3 in flight by a moodycamel LightweightSemaphore of
    // 1e3 slots (producer waits a slot then enqueues; a consumer signals one per
    // item it takes). moodycamel has no bounded blocking enqueue of its own, and
    // try_enqueue on its preallocated blocks deadlocked here (measured
    // 2026-10-25: a producer saw the queue full while the consumer saw it
    // empty), so the bound is the semaphore. The queue orders per producer
    // only, so a sentinel could overtake data: consumers stop on the count, and
    // once all N are taken main enqueues one 0 per consumer to wake any still
    // blocked in wait_dequeue (messages are 1..N, so 0 is never data).
    struct Shape { const char* id; int p, c; };
    for (Shape sh : {Shape{"channel-spsc", 1, 1}, Shape{"channel-mpsc", 4, 1}, Shape{"channel-mpmc", 4, 4}}) {
        LIBVER = "1.0.4";
        bench(sh.id, "moodycamel::BlockingConcurrentQueue", NCH,
              join("1e3 in flight via a 1e3-slot LightweightSemaphore; ProducerToken per producer; OS threads", smoke_note("1e4 messages")), [&]{
            moodycamel::BlockingConcurrentQueue<i64> q(1000, sh.p + 1, 1);
            moodycamel::LightweightSemaphore slots(1000);
            std::atomic<i64> sum{0}, got{0}, taken{0};
            std::atomic<bool> done{false};
            i64 per = NCH / sh.p;
            std::vector<std::thread> cons;
            for (int c = 0; c < sh.c; ++c) cons.emplace_back([&]{
                i64 s = 0, n = 0, v;
                while (!done.load()) {
                    q.wait_dequeue(v);
                    if (v == 0) continue;
                    slots.signal();
                    s += v; ++n;
                    if (taken.fetch_add(1) + 1 == NCH) done.store(true);
                }
                sum += s; got += n;
            });
            std::atomic<i64> fails{0};
            run_threads(sh.p, [&](int p){
                moodycamel::ProducerToken tok(q);
                for (i64 i = 0; i < per; ++i) { slots.wait(); if (!q.enqueue(tok, (i64)p * per + i + 1)) fails.fetch_add(1); }
            });
            while (!done.load()) std::this_thread::yield();
            for (int c = 0; c < sh.c; ++c) q.enqueue(0);
            for (auto& t : cons) t.join();
            return (got.load() == NCH && fails.load() == 0) ? sum.load() : -1;
        }, [&](i64 r){ return r == NCH * (NCH + 1) / 2; });
    }

    LIBVER = "std";
    skip_row("fiber-sleep-wake", 1, "Fiber.md lists no C++ competitor (std::this_thread::sleep_for is an OS-thread sleep)");

    bench("fiberlocal-get-set", "thread_local", N1,
          join("tl = i then sum += tl per pass with a compiler fence so both happen", s1), [&]{
        i64 sum = 0;
        for (i64 i = 0; i < N1; ++i) {
            TL_SLOT = i;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            sum += TL_SLOT;
        }
        return sum;
    }, [&](i64 r){ return r == N1 * (N1 - 1) / 2; });

    bench("task-spawn-await", "std::async", NSP, join("std::async(launch::async) + get: one OS thread per task", smoke_note("1e4 tasks")), [&]{
        i64 sum = 0;
        for (i64 i = 0; i < NSP; ++i) sum += std::async(std::launch::async, [i]{ return i + 1; }).get();
        return sum;
    }, [&](i64 r){ return r == NSP * (NSP + 1) / 2; });
    return 0;
}
