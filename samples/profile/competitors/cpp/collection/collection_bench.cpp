// C++ collection competitors — the same four workloads the Cajeta side runs:
// hashmap-int / hashmap-string (Ankerl insert+find shape), hashset-dedup,
// arraylist-append. One schema-conformant CSV row per (benchmark, library).
// Cross-checks are the exact closed forms. Libraries: std::unordered_map (the
// baseline) and ankerl::unordered_dense (the benchmark-winning flat map, vendored
// single header); std::unordered_set + std::vector for dedup/append.
#include "ankerl/unordered_dense.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using clk = std::chrono::steady_clock;
static const int N_INT = 50000, N_STR = 30000, N_SET = 100000, SET_HALF = 50000, N_VEC = 100000;
static const long long INT_REF = 2499950000LL;
static const long long STR_REF = 449985000LL;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
#include <atomic>
#include <cstdlib>
#include <new>
static std::atomic<unsigned long long> _ALLOCED{0};
static std::atomic<unsigned long long> _LASTALLOC{0};
void* operator new(std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
template <class F> static void _alloc_of(F run) { _ALLOCED = 0; run(); _LASTALLOC = _ALLOCED.load(); }
static unsigned long long _la() { return _LASTALLOC.load(); }

static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 const char* lib, int input, int warmup, int trials,
                 std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,collection,,%d,,%d,cpp,%s,%s,std,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), (unsigned long long)_la(),
        ok ? "ok" : "invalid", ok ? "true" : "false");
}

template <class F, class C>
static std::vector<long long> bench(int warmup, int trials, F f, C check, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto r = f(); (void)r; }
    std::vector<long long> s; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto t0 = clk::now();
        auto r = f();
        auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = check(r);
    }
    _alloc_of([&]{ f(); });
    return s;
}

template <class Map>
static long long hashmap_int() {
    Map m; m.reserve(N_INT);
    for (long long i = 0; i < N_INT; ++i) m[i] = i * 2;
    long long sum = 0;
    for (long long j = 0; j < N_INT; ++j) sum += m[j];
    return sum;
}
template <class Map>
static long long hashmap_str() {
    Map m; m.reserve(N_STR);
    for (long long i = 0; i < N_STR; ++i) m["key" + std::to_string(i)] = i;
    long long sum = 0;
    for (long long j = 0; j < N_STR; ++j) sum += m["key" + std::to_string(j)];
    return sum;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    bool ok;

    // hashmap-int
    auto s = bench(warmup, trials, []{ return hashmap_int<std::unordered_map<long long, long long>>(); },
                   [&](long long r){ return r == INT_REF; }, ok);
    emit(run_id, ts, "hashmap-int", "std::unordered_map", N_INT, warmup, trials, s, ok);
    s = bench(warmup, trials, []{ return hashmap_int<ankerl::unordered_dense::map<long long, long long>>(); },
              [&](long long r){ return r == INT_REF; }, ok);
    emit(run_id, ts, "hashmap-int", "ankerl::unordered_dense", N_INT, warmup, trials, s, ok);

    // hashmap-string
    s = bench(warmup, trials, []{ return hashmap_str<std::unordered_map<std::string, long long>>(); },
              [&](long long r){ return r == STR_REF; }, ok);
    emit(run_id, ts, "hashmap-string", "std::unordered_map", N_STR, warmup, trials, s, ok);
    s = bench(warmup, trials, []{ return hashmap_str<ankerl::unordered_dense::map<std::string, long long>>(); },
              [&](long long r){ return r == STR_REF; }, ok);
    emit(run_id, ts, "hashmap-string", "ankerl::unordered_dense", N_STR, warmup, trials, s, ok);

    // hashset-dedup
    s = bench(warmup, trials, []{
        std::unordered_set<long long> set;
        for (long long i = 0; i < N_SET; ++i) set.insert(i % SET_HALF);
        return (int)set.size();
    }, [&](int r){ return r == SET_HALF; }, ok);
    emit(run_id, ts, "hashset-dedup", "std::unordered_set", N_SET, warmup, trials, s, ok);

    // arraylist-append
    s = bench(warmup, trials, []{
        std::vector<long long> v;
        for (long long i = 0; i < N_VEC; ++i) v.push_back(i);
        return (int)v.size();
    }, [&](int r){ return r == N_VEC; }, ok);
    emit(run_id, ts, "arraylist-append", "std::vector", N_VEC, warmup, trials, s, ok);
    return 0;
}
