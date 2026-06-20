// C++ time competitors — the same two workloads the Cajeta side runs:
// time-instant-arith (1M) and time-localdate-arith (100K), using C++20
// std::chrono (sys_seconds / sys_days). One schema-conformant CSV row per
// benchmark. Cross-check = the exact closed-form sum (500006500000 / 5000050000).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
namespace cr = std::chrono;
static const int N_INSTANT = 1000000;
static const int N_DATE = 100000;
static const long long INSTANT_REF = 500006500000LL;
static const long long DATE_REF = 5000050000LL;

// Force the compiler to materialize a value each iteration — otherwise g++ -O3
// recognizes the epoch arithmetic as a closed-form series (Gauss) and eliminates
// the entire loop, reporting absurd throughput.
template <class T>
static inline void do_not_optimize(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

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
                 int input, int warmup, int trials, std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mops = med > 0 ? (double)input / (double)med * 1e9 / 1e6 : 0.0;
    std::printf(
        "1,%s,%s,%s,time,,%d,,%d,cpp,%s,std::chrono,std,-O3 -march=native -std=c++20,%d,%d,"
        "%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), (unsigned long long)_la(), ok ? "ok" : "invalid", ok ? "true" : "false");
}

template <class F, class C>
static std::vector<long long> bench(int warmup, int trials, F f, C check, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto r = f(); (void)r; }
    std::vector<long long> s; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto t0 = clk::now(); auto r = f(); auto t1 = clk::now();
        s.push_back(cr::duration_cast<cr::nanoseconds>(t1 - t0).count());
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
    // Runtime base (0 in practice) read from the environment so the compiler can't
    // constant-fold the whole loop — std::chrono arithmetic on compile-time-known
    // values is constexpr and would otherwise vanish, reporting absurd throughput.
    long long base = std::atoll(env("PROFILE_TIME_BASE", "0").c_str());
    bool ok;

    // instant-arith: sys_seconds{i} + 7s, read epoch seconds
    auto s1 = bench(warmup, trials, [base]{
        long long sum = 0;
        for (long long i = 0; i < N_INSTANT; ++i) {
            cr::sys_seconds t{cr::seconds{i + base}};
            cr::sys_seconds t2 = t + cr::seconds{7};
            do_not_optimize(t2);
            sum += t2.time_since_epoch().count();
        }
        return sum;
    }, [&](long long r){ return r == INSTANT_REF; }, ok);
    emit(run_id, ts, "time-instant-arith", N_INSTANT, warmup, trials, s1, ok);

    // localdate-arith: sys_days{i} + 1 day, read epoch days
    auto s2 = bench(warmup, trials, [base]{
        long long sum = 0;
        for (long long i = 0; i < N_DATE; ++i) {
            cr::sys_days d{cr::days{i + base}};
            cr::sys_days d2 = d + cr::days{1};
            do_not_optimize(d2);
            sum += d2.time_since_epoch().count();
        }
        return sum;
    }, [&](long long r){ return r == DATE_REF; }, ok);
    emit(run_id, ts, "time-localdate-arith", N_DATE, warmup, trials, s2, ok);
    return 0;
}
