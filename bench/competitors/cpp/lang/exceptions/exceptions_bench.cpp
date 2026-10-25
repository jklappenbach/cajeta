// C++ competitor: unit-5 lang/exceptions benchmarks (contract: u5-competitor-spec.md).
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
#include <exception>

struct BenchError : std::exception {
    const char* what() const noexcept override { return "bench error"; }
};


__attribute__((noinline)) static int64_t f_maybe_throw(int64_t i, int64_t mask) {
    if (i < 0) throw BenchError();
    return i & mask;
}

// depth frames of non-inlined recursion; the innermost throws when i >= 0 (always).
// The empty asm after the recursive call keeps each frame live (no tail call / loop).
__attribute__((noinline)) static int64_t thrower(int depth, int64_t i) {
    if (depth <= 1) {
        if (i >= 0) throw BenchError();
        return i;
    }
    int64_t r = thrower(depth - 1, i);
    asm volatile("" : "+r"(r));
    return r + 1;
}

static int64_t throw_catch(int depth, int64_t iters) {
    int64_t count = 0;
    for (int64_t i = 0; i < iters; ++i) {
        try { int64_t r = thrower(depth, i); count -= r; }  // never reached
        catch (const BenchError&) { ++count; }
    }
    return count;
}

int main() {
    Ctx ctx; CTX = &ctx;
    const int64_t N = 100000000;

    run("try-no-throw", N, "try/catch", "std", "", "", [&] {
        const int64_t mask = V_MASK; int64_t s = 0;
        for (int64_t i = 0; i < N; ++i) {
            try { s += f_maybe_throw(i, mask); }
            catch (const BenchError&) { s -= 1; }
        }
        return s;
    }, [](int64_t s) { return s == 4999999950000000LL; });

    run("throw-catch-1", 1000000, "throw/catch", "std", "", "", [&] { return throw_catch(1, 1000000); },
        [](int64_t c) { return c == 1000000; });
    run("throw-catch-10", 1000000, "throw/catch", "std", "", "", [&] { return throw_catch(10, 1000000); },
        [](int64_t c) { return c == 1000000; });
    run("throw-catch-100", 100000, "throw/catch", "std", "", "", [&] { return throw_catch(100, 100000); },
        [](int64_t c) { return c == 100000; });

    emit_skipped("throw-recoverable", 1000000, "throw/catch", "std",
                 "no recoverable/unrecoverable distinction; the page's competitor is Java", "");
    emit_skipped("throw-in-fiber", 100000, "throw/catch", "std", "C++ has no standard fiber or green thread", "");
    return 0;
}
