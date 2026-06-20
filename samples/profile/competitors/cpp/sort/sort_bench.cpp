// C++ sort competitors — sort 50K int64 across the same four input patterns the
// Cajeta side uses (random / ascending / descending / dups), plus a 50K double
// random sort. One schema-conformant CSV row per (benchmark, variant, library).
// Same splitmix64 input sequence as every other language. Cross-check: output
// non-decreasing and (for int64) wrapping sum preserved. Libraries: std::sort
// (introsort), pdqsort (vendored single header — pattern-defeating), and
// std::stable_sort.
#include "pdqsort.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static const size_t N = 50000;
static const uint64_t SEED = 0x123456789ABCDEF0ULL;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

struct Split { uint64_t s;
    uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL; uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

static std::vector<int64_t> make_i64(const std::string& variant) {
    Split g{SEED}; std::vector<int64_t> v(N);
    for (size_t i = 0; i < N; ++i) {
        if (variant == "random") v[i] = (int64_t)g.next();
        else if (variant == "ascending") v[i] = (int64_t)i;
        else if (variant == "descending") v[i] = (int64_t)(N - 1 - i);
        else v[i] = (int64_t)(i % 16); // dups
    }
    return v;
}
static std::vector<double> make_f64() {
    Split g{SEED}; std::vector<double> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = (double)(g.next() % 100000000ULL) / 7.0;
    return v;
}
static uint64_t wsum(const std::vector<int64_t>& v) {
    uint64_t a = 0; for (int64_t x : v) a += (uint64_t)x; return a;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 const char* variant, const char* lib, int warmup, int trials,
                 std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mels = med > 0 ? (double)N / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,sort,,%zu,,%zu,cpp,%s,%s,std,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.2f,Melem/s,%lld,-1,-1,-1,-1,%s,%s,,%s\n",
        run_id.c_str(), ts.c_str(), bench, N, N, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, warmup, trials, mn, med, mean, p95, mels, peak_rss_kb(),
        ok ? "ok" : "invalid", ok ? "true" : "false", variant);
}

template <class Sorter>
static std::vector<long long> bench_i64(const std::vector<int64_t>& input, int warmup,
                                        int trials, Sorter sorter, bool& ok, uint64_t want) {
    for (int i = 0; i < warmup; ++i) { auto w = input; sorter(w); }
    std::vector<long long> samples; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto w = input;
        auto t0 = clk::now(); sorter(w); auto t1 = clk::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = std::is_sorted(w.begin(), w.end()) && wsum(w) == want;
    }
    return samples;
}

template <class Sorter>
static std::vector<long long> bench_f64(const std::vector<double>& input, int warmup,
                                        int trials, Sorter sorter, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto w = input; sorter(w); }
    std::vector<long long> samples; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto w = input;
        auto t0 = clk::now(); sorter(w); auto t1 = clk::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = std::is_sorted(w.begin(), w.end());
    }
    return samples;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    const char* variants[] = {"random", "ascending", "descending", "dups"};

    for (const char* variant : variants) {
        auto input = make_i64(variant);
        uint64_t want = wsum(input);
        bool ok;
        auto s1 = bench_i64(input, warmup, trials,
            [](std::vector<int64_t>& w){ std::sort(w.begin(), w.end()); }, ok, want);
        emit(run_id, ts, "sort-int64", variant, "std::sort", warmup, trials, s1, ok);
        auto s2 = bench_i64(input, warmup, trials,
            [](std::vector<int64_t>& w){ pdqsort(w.begin(), w.end()); }, ok, want);
        emit(run_id, ts, "sort-int64", variant, "pdqsort", warmup, trials, s2, ok);
    }
    // stable (random only)
    {
        auto input = make_i64("random"); uint64_t want = wsum(input); bool ok;
        auto s = bench_i64(input, warmup, trials,
            [](std::vector<int64_t>& w){ std::stable_sort(w.begin(), w.end()); }, ok, want);
        emit(run_id, ts, "sort-stable-int64", "", "std::stable_sort", warmup, trials, s, ok);
    }
    // f64 random
    {
        auto input = make_f64(); bool ok;
        auto s = bench_f64(input, warmup, trials,
            [](std::vector<double>& w){ std::sort(w.begin(), w.end()); }, ok);
        emit(run_id, ts, "sort-f64", "", "std::sort", warmup, trials, s, ok);
    }
    return 0;
}
