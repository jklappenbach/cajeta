// C++ competitor: unit-5 lang/loops benchmarks (contract: u5-competitor-spec.md).
// ---- common harness (identical in every unit-5 cpp lang program) ----
// One v1-shape CSV row (32 columns) per (benchmark, variant, library). Each
// workload returns its result; the check is applied to every timed trial's
// result, so nothing can be dead-code eliminated. OPAQUE values are read
// through volatiles; launder() is an empty asm the optimizer cannot see through.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static const char* FLAGS = "-O3 -march=native -std=c++23";
static volatile int64_t V_MASK = -1;  // OPAQUE mask
static volatile int64_t V_ONE = 1;    // OPAQUE one
static volatile int WARM_SINK = 0;

template <class T> static inline T launder(T v) { asm volatile("" : "+r"(v) : : "memory"); return v; }

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }

static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

struct Ctx {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    std::string lang_version = env("PROFILE_LANG_VERSION", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    std::vector<std::string> only;
    Ctx() {
        std::string o = env("LANG_ONLY", ""); std::stringstream ss(o); std::string t;
        while (std::getline(ss, t, ',')) if (!t.empty()) only.push_back(t);
        if (trials < 1) trials = 1;
        if (warmup < 0) warmup = 0;
    }
    bool selected(const char* slug) const {
        if (only.empty()) return true;
        return std::find(only.begin(), only.end(), std::string(slug)) != only.end();
    }
};
static Ctx* CTX;

static void emit(const char* slug, long long input, const char* lib, const char* libver,
                 std::vector<long long> s, bool ok, const char* notes, const char* variant) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf("1,%s,%s,%s,lang,,%lld,,%lld,cpp,%s,%s,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,-1,-1,-1,%s,%s,%s,%s,cpu\n",
        CTX->run_id.c_str(), CTX->ts.c_str(), slug, input, input, CTX->lang_version.c_str(), lib, libver, FLAGS,
        CTX->warmup, CTX->trials, mn, med, mean, p95, mops, peak_rss_kb(), ok ? "ok" : "invalid", ok ? "true" : "false",
        notes, variant);
    std::fflush(stdout);
}

static void emit_skipped(const char* slug, long long input, const char* lib, const char* libver,
                         const char* reason, const char* variant) {
    if (!CTX->selected(slug)) return;
    std::printf("1,%s,%s,%s,lang,,%lld,,%lld,cpp,%s,%s,%s,%s,%d,%d,-1,-1,-1,-1,,Mop/s,%lld,-1,-1,-1,-1,skipped,,%s,%s,cpu\n",
        CTX->run_id.c_str(), CTX->ts.c_str(), slug, input, input, CTX->lang_version.c_str(), lib, libver, FLAGS,
        CTX->warmup, CTX->trials, peak_rss_kb(), reason, variant);
    std::fflush(stdout);
}

// run(): warmup untimed runs, then timed trials; check every trial's result.
template <class F, class C>
static void run(const char* slug, long long input, const char* lib, const char* libver,
                const char* notes, const char* variant, F f, C check) {
    if (!CTX->selected(slug)) return;
    for (int i = 0; i < CTX->warmup; ++i) { auto r = f(); WARM_SINK = WARM_SINK + (check(r) ? 1 : 0); }
    std::vector<long long> s; bool ok = true;
    for (int i = 0; i < CTX->trials; ++i) {
        auto t0 = clk::now(); auto r = f(); auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (!check(r)) ok = false;
    }
    emit(slug, input, lib, libver, s, ok, notes, variant);
}
// ---- end common harness ----
#include <ranges>
#include <cmath>
#include <utility>

int main() {
    Ctx ctx; CTX = &ctx;
    const int64_t N = 100000000;

    run("loop-counted", N, "for", "std", "", "", [&] {
        const int64_t mask = V_MASK; int64_t s = 0;
        for (int64_t i = 0; i < N; ++i) s += i & mask;
        return s;
    }, [](int64_t s) { return s == 4999999950000000LL; });

    {
        bool need = ctx.selected("loop-counted-nobounds");
        std::vector<int32_t> a(need ? 1000000 : 0);
        for (int32_t k = 0; k < (int32_t)a.size(); ++k) a[k] = k;
        run("loop-counted-nobounds", 100 * 1000000LL, "vector::at", "std", "", "bounds-on", [&] {
            int64_t s = 0; const size_t n = a.size();
            for (int p = 0; p < 100; ++p) for (size_t k = 0; k < n; ++k) s += a.at(k);
            return s;
        }, [](int64_t s) { return s == 49999950000000LL; });
        run("loop-counted-nobounds", 100 * 1000000LL, "vector::operator[]", "std", "", "bounds-off", [&] {
            int64_t s = 0; const size_t n = a.size();
            for (int p = 0; p < 100; ++p) for (size_t k = 0; k < n; ++k) s += a[k];
            return s;
        }, [](int64_t s) { return s == 49999950000000LL; });
    }

    run("loop-nested", 100000000LL, "for", "std", "", "", [&] {
        int64_t s = 0;
        for (int64_t i = 0; i < 10000; ++i) for (int64_t j = 0; j < 10000; ++j) s += i ^ j;
        return s;
    }, [](int64_t s) { return s == 642122061696LL; });

    if (ctx.selected("loop-foreach-array") || ctx.selected("loop-foreach-arraylist")) {
        std::vector<int32_t> a(10000000);
        for (int32_t k = 0; k < 10000000; ++k) a[k] = k % 1000;
        run("loop-foreach-array", 10000000LL, "vector range-for", "std", "", "", [&] {
            int64_t s = 0; for (int32_t x : a) s += x; return s;
        }, [](int64_t s) { return s == 4995000000LL; });
        std::vector<int32_t> l;
        for (int32_t k = 0; k < 10000000; ++k) l.push_back(k % 1000);
        run("loop-foreach-arraylist", 10000000LL, "vector push_back range-for", "std",
            "same std::vector type as loop-foreach-array; built by push_back", "", [&] {
            int64_t s = 0; for (int32_t x : l) s += x; return s;
        }, [](int64_t s) { return s == 4995000000LL; });
    }

    run("loop-range", N, "views::iota", "std", "", "", [&] {
        const int64_t mask = V_MASK; int64_t s = 0;
        for (int64_t i : std::views::iota(0LL, (long long)N)) s += i & mask;
        return s;
    }, [](int64_t s) { return s == 4999999950000000LL; });

    if (ctx.selected("loop-reduce")) {
        std::vector<double> x(10000000);
        for (int k = 0; k < 10000000; ++k) x[k] = (k % 1000) * 0.5;
        run("loop-reduce", 10000000LL, "for", "std", "sequential; no fast-math", "", [&] {
            double s = 0.0; for (size_t k = 0; k < x.size(); ++k) s += x[k]; return s;
        }, [](double s) { return std::fabs(s - 2497500000.0) <= 1e-6 * 2497500000.0; });
    }

    if (ctx.selected("loop-early-exit")) {
        std::vector<int32_t> a(10000000);
        for (int32_t k = 0; k < 10000000; ++k) a[k] = k;
        a[5000000] = -1;
        const int32_t* base = a.data(); const int64_t n = (int64_t)a.size();
        run("loop-early-exit", 1000LL * 10000000LL, "for+break", "std", "", "", [&] {
            int64_t s = 0;
            for (int r = 0; r < 1000; ++r) {
                const int32_t* p = launder(base);  // re-scan each repetition; no hoisting of the search
                int64_t found = -1;
                for (int64_t k = 0; k < n; ++k) { if (p[k] < 0) { found = k; break; } }
                s += found;
            }
            return s;
        }, [](int64_t s) { return s == 5000000000LL; });
    }
    return 0;
}
