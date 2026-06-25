// C++ stream competitors — the same two workloads the Cajeta side runs over
// xs[i] = i % 1000 (N=1M): stream-filter-map-reduce and stream-parallel-reduce.
// To compare like-for-like against Cajeta's Stream abstraction (a lazy pipeline,
// not a hand-fused loop), these use the EQUIVALENT C++ high-level abstractions:
// std::views::filter|transform + std::ranges::fold_left for filter-map-reduce,
// and std::reduce(std::execution::par, …) for parallel-reduce — NOT a hand loop
// or a raw OpenMP reduction. One schema-conformant CSV row per benchmark.
// Cross-check = the exact reference sums (250000000 / 499500000).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <execution>
#include <fstream>
#include <functional>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static const int N = 1000000;
static const long long FMR_REF = 250000000LL;
static const long long PR_REF = 499500000LL;

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
                 const char* lib, int input, int warmup, int trials, std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,stream,,%d,,%d,cpp,%s,%s,std,-O3 -march=native -std=c++23,%d,%d,"
        "%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), (unsigned long long)_la(), ok ? "ok" : "invalid", ok ? "true" : "false");
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
    _alloc_of([&]{ f(); });
    return s;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    std::vector<long long> xs(N);
    for (int i = 0; i < N; ++i) xs[i] = i % 1000;
    bool ok;

    // filter-map-reduce — a lazy view pipeline (filter evens → map +1) folded to
    // a sum, the C++ analogue of Cajeta's Stream.filter().map().reduce(). std::
    // ranges::fold_left sinks the result, so the pipeline can't be DCE'd.
    auto s1 = bench(warmup, trials, [&]{
        auto pipe = xs
            | std::views::filter([](long long x){ return x % 2 == 0; })
            | std::views::transform([](long long x){ return x + 1; });
        return std::ranges::fold_left(pipe, 0LL, std::plus<long long>{});
    }, [&](long long r){ return r == FMR_REF; }, ok);
    emit(run_id, ts, "stream-filter-map-reduce", "views::filter|transform", N, warmup, trials, s1, ok);

    // parallel-reduce — the standard parallel algorithm (libstdc++ → TBB), the
    // analogue of Cajeta's parallel Stream reduce. The returned sum is checked,
    // so it can't be DCE'd.
    auto s2 = bench(warmup, trials, [&]{
        return std::reduce(std::execution::par, xs.begin(), xs.end(), 0LL);
    }, [&](long long r){ return r == PR_REF; }, ok);
    emit(run_id, ts, "stream-parallel-reduce", "std::reduce(par)", N, warmup, trials, s2, ok);
    return 0;
}
