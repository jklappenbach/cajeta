// C++ concurrency competitors — the same two workloads the Cajeta side runs:
// atomic-fetchadd (1M single-threaded fetch_add on std::atomic<long long>) and
// task-spawn-await (20K: std::thread spawn + join per task, accumulate i+1). One
// schema-conformant CSV row per benchmark. Cross-check = the exact closed forms
// (1000000 / 200010000). The task primitive is an OS thread (no async runtime).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static const int N_ATOMIC = 1000000;
static const int N_SPAWN = 20000;
static const long long SPAWN_REF = 200010000LL;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 const char* lib, int input, int warmup, int trials, std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,concurrent,,%d,,%d,cpp,%s,%s,std,-O3 -march=native -pthread,%d,%d,"
        "%lld,%lld,%lld,%lld,%.3f,Mop/s,%lld,-1,-1,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), ok ? "ok" : "invalid", ok ? "true" : "false");
}

template <class F, class C>
static std::vector<long long> bench(int warmup, int trials, F f, C check, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto r = f(); (void)r; }
    std::vector<long long> s; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto t0 = clk::now(); auto r = f(); auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = check(r);
    }
    return s;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    bool ok;

    // atomic-fetchadd
    auto s1 = bench(warmup, trials, []{
        std::atomic<long long> a{0};
        for (int i = 0; i < N_ATOMIC; ++i) a.fetch_add(1, std::memory_order_relaxed);
        return a.load(std::memory_order_relaxed);
    }, [&](long long r){ return r == N_ATOMIC; }, ok);
    emit(run_id, ts, "atomic-fetchadd", "std::atomic", N_ATOMIC, warmup, trials, s1, ok);

    // task-spawn-await (OS thread spawn + join per task)
    auto s2 = bench(warmup, trials, []{
        long long sum = 0;
        for (long long i = 0; i < N_SPAWN; ++i) {
            long long out = 0;
            std::thread th([i, &out]{ out = i + 1; });
            th.join();
            sum += out;
        }
        return sum;
    }, [&](long long r){ return r == SPAWN_REF; }, ok);
    emit(run_id, ts, "task-spawn-await", "std::thread", N_SPAWN, warmup, trials, s2, ok);
    return 0;
}
