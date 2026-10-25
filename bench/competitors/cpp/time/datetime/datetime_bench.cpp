// C++ time competitors (benchmark-reference unit 8): the time/ ids the Cajeta
// side runs in bench/src/main/cajeta/bench/time/, same sizes, same inputs and
// the same check constants. Library: C++20 std::chrono from libstdc++ 13
// (durations, calendar, zoned_time against the system tz database, and
// std::format for chrono); parsing uses Howard Hinnant's date (pinned in
// competitors/pins.tsv) because libstdc++ 13 has no std::chrono::parse (GCC 14
// adds it). C++ has no time-of-day type: localtime-arith uses chrono seconds
// modulo days{1}. `opaque` hides each loop input from the optimizer so g++
// cannot fold a loop into its closed form; every checksum is checked.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <new>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "date/date.h"

namespace cr = std::chrono;
using clk = cr::steady_clock;

static const long long N = 10000000, N_ZONE = 1000000, N_FMT = 1000000;
static const long long DURATION_REF = 159999997999999LL;
static const long long LOCALTIME_REF = 432435641193LL;
static const long long LOCALDATETIME_REF = 20264337423627200LL;
static const long long ZONE_REF = -25266031177LL;
static const char* FIRST_ISO = "2024-01-01T00:00:00.000000001Z";

template <class T> static inline T opaque(T v) { asm volatile("" : "+r"(v)); return v; }
template <class T> static inline void sink(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

static std::atomic<unsigned long long> ALLOCED{0};
void* operator new(std::size_t n) { ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v && *v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) { long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb; }
    return -1;
}
static std::string clean(std::string s) { for (auto& c : s) if (c == ',' || c == '\n' || c == '"') c = ';'; return s; }

struct Cfg { std::string run_id, ts, ver; int warmup, trials; };

static void row(const Cfg& c, const char* bench, long long n, const char* lib, const char* libver,
                std::vector<long long> s, unsigned long long alloc, const char* status, bool ok, const std::string& notes) {
    long long mn = -1, med = -1, mean = -1, p95 = -1;
    if (!s.empty()) {
        std::sort(s.begin(), s.end()); size_t k = s.size();
        mn = s[0]; med = s[k / 2]; mean = std::accumulate(s.begin(), s.end(), 0LL) / (long long)k; p95 = s[std::min(k - 1, k * 95 / 100)];
    }
    double mops = med > 0 ? (double)n / (double)med * 1e3 : 0.0;
    std::printf("1,%s,%s,%s,time,,%lld,,%lld,cpp,%s,%s,%s,-O3 -march=native -std=c++20,%d,%d,%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,%s,\n",
        c.run_id.c_str(), c.ts.c_str(), bench, n, n, c.ver.c_str(), lib, libver, c.warmup, c.trials,
        mn, med, mean, p95, mops, peak_rss_kb(), alloc, status, ok ? "true" : "false", clean(notes).c_str());
}

static void measure(const Cfg& c, const char* bench, long long n, const char* lib, const char* libver, const char* notes,
                    const std::function<long long()>& f, const std::function<bool(long long)>& check) {
    try {
        for (int w = 0; w < c.warmup; w++) sink(f());
        std::vector<long long> s; bool ok = true;
        for (int t = 0; t < c.trials; t++) {
            auto t0 = clk::now(); long long r = f(); auto t1 = clk::now();
            sink(r); ok = ok && check(r);
            s.push_back(cr::duration_cast<cr::nanoseconds>(t1 - t0).count());
        }
        ALLOCED = 0; sink(f()); unsigned long long a = ALLOCED.load();
        row(c, bench, n, lib, libver, s, a, ok ? "ok" : "invalid", ok, notes);
    } catch (const std::exception& e) {
        row(c, bench, n, lib, libver, {}, 0, "failed", false, e.what());
    }
}

static long long fmt_second(long long i) { return 1704067200LL + i * 997; }
static long long fmt_nano(long long i) { return ((i * 1000003LL) % 100000000LL) * 10 + 1; }
static cr::sys_time<cr::nanoseconds> fmt_instant(long long i) {
    return cr::sys_time<cr::nanoseconds>{cr::seconds{fmt_second(i)} + cr::nanoseconds{fmt_nano(i)}};
}

static long long clock_now() {
    auto first = clk::now(), prev = first; long long back = 0;
    for (long long i = 1; i < N; i++) { auto t = clk::now(); if (t < prev) back++; prev = t; sink(t); }
    return back == 0 && prev > first ? 1 : 0;
}

static long long duration_arith() {
    using namespace std::chrono_literals;
    const cr::nanoseconds three = 3ms, limit = 10ms;
    long long acc = 0, cnt = 0;
    for (long long i = 0; i < N; i++) {
        cr::nanoseconds d = (cr::nanoseconds{opaque(i)} + three) * 2;
        acc += d.count();
        if (d > limit) cnt++;
    }
    return acc + cnt;
}

static long long localtime_arith() {
    using namespace std::chrono_literals;
    const cr::seconds step{18007}, noon = 12h;
    long long acc = 0, cnt = 0;
    for (long long i = 0; i < N; i++) {
        cr::seconds t{opaque(i) % 86400};
        cr::seconds u = (t + step) % cr::days{1};
        acc += u.count();
        if (u < noon) cnt++;
    }
    return acc + cnt;
}

static long long localdatetime_arith() {
    const cr::local_seconds base = cr::local_days{cr::year{2024} / 1 / 1};
    long long acc = 0;
    for (long long i = 0; i < N; i++) {
        cr::local_seconds x = base + cr::hours{opaque(i) % 50000};
        cr::local_days dp = cr::floor<cr::days>(x);
        cr::year_month_day ymd{dp};
        long long hh = cr::duration_cast<cr::hours>(x - dp).count();
        acc += (long long)(int)ymd.year() * 1000000 + (long long)(unsigned)ymd.month() * 10000
             + (long long)(unsigned)ymd.day() * 100 + hh;
    }
    return acc;
}

static long long zoned_region() {
    const cr::time_zone* tz = cr::locate_zone("America/Los_Angeles");
    long long acc = 0;
    for (long long i = 0; i < N_ZONE; i++) {
        cr::sys_seconds t{cr::seconds{1577836800LL + opaque(i) * 307}};
        cr::zoned_time zt{tz, t};
        cr::local_seconds lt = zt.get_local_time();
        long long off = zt.get_info().offset.count();
        cr::local_days dp = cr::floor<cr::days>(lt);
        cr::year_month_day ymd{dp};
        long long hh = cr::duration_cast<cr::hours>(lt - dp).count();
        acc += off + hh * 100 + (long long)(unsigned)ymd.day();
    }
    return acc;
}

static long long format_iso() {
    long long len = 0; bool first_ok = false;
    for (long long i = 0; i < N_FMT; i++) {
        std::string s = std::format("{:%FT%T}Z", fmt_instant(opaque(i)));
        if (i == 0) first_ok = s == FIRST_ISO;
        len += (long long)s.size();
        sink(s.data());
    }
    return first_ok ? len : -1;
}

static long long parse_iso(const std::vector<std::string>& inp) {
    long long good = 0;
    std::istringstream in;
    for (size_t i = 0; i < inp.size(); i++) {
        in.clear(); in.str(inp[i]);
        date::sys_time<cr::nanoseconds> tp;
        in >> date::parse("%FT%TZ", tp);
        if (in.fail()) throw std::runtime_error("date::parse failed on " + inp[i]);
        if (tp == fmt_instant((long long)i)) good++;
    }
    return good;
}

int main() {
    Cfg c{env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""),
          std::atoi(env("PROFILE_WARMUP", "3").c_str()), std::atoi(env("PROFILE_TRIALS", "10").c_str())};
    const std::string svs = "libstdc++-" + std::to_string(_GLIBCXX_RELEASE); const char* sv = svs.c_str();
    measure(c, "clock-now", N, "std::chrono", sv, "steady_clock::now", clock_now, [](long long r) { return r == 1; });
    measure(c, "duration-arith", N, "std::chrono", sv, "chrono::nanoseconds", duration_arith, [](long long r) { return r == DURATION_REF; });
    measure(c, "localtime-arith", N, "std::chrono", sv, "no time-of-day type: chrono::seconds modulo days{1}", localtime_arith, [](long long r) { return r == LOCALTIME_REF; });
    measure(c, "localdatetime-arith", N, "std::chrono", sv, "local_seconds + hours; year_month_day", localdatetime_arith, [](long long r) { return r == LOCALDATETIME_REF; });
    measure(c, "zoneddatetime-region", N_ZONE, "std::chrono", sv, "zoned_time (libstdc++ tzdb)", zoned_region, [](long long r) { return r == ZONE_REF; });
    measure(c, "datetime-format-iso", N_FMT, "std::chrono", sv, "std::format {:%FT%T}Z", format_iso, [](long long r) { return r == N_FMT * 30; });
    std::vector<std::string> inp;
    inp.reserve(N_FMT);
    for (long long i = 0; i < N_FMT; i++) inp.push_back(std::format("{:%FT%T}Z", fmt_instant(i)));
    measure(c, "datetime-parse-iso", N_FMT, "date", "3.0.4", "date::parse %FT%TZ (libstdc++ 13 has no chrono::parse)",
            [&] { return parse_iso(inp); }, [](long long r) { return r == N_FMT; });
    return 0;
}
