// Row emitter for the C++ io competitor (competitors/drivers/io/io.sh).
// Prints v1 rows (31 columns, competitors/columns.txt order) with area io;
// lib/rows.sh's rows_v2 lifts them to v2.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace row {

inline std::string env(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string(d);
}
inline bool smoke() {
    std::string s = env("CAJETA_BENCH_SMOKE", "");
    return s == "1" || s == "true" || s == "yes";
}
inline long long pick(long long full, long long sm) { return smoke() ? sm : full; }
inline int env_int(const char* k, int d) { return std::atoi(env(k, std::to_string(d).c_str()).c_str()); }

inline long long status_kb(const char* key) {
    std::ifstream s("/proc/self/status");
    std::string line;
    size_t kl = std::string(key).size();
    while (std::getline(s, line)) {
        if (line.compare(0, kl, key) == 0) return std::atoll(line.c_str() + kl);
    }
    return -1;
}
inline long long peak_rss_kb() { return status_kb("VmHWM:"); }
inline long long rss_kb() { return status_kb("VmRSS:"); }

using clk = std::chrono::steady_clock;
inline long long now_ns() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
}

struct Stats { long long mn = -1, med = -1, mean = -1, p95 = -1; };
inline Stats stats(std::vector<long long> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    long long sum = std::accumulate(v.begin(), v.end(), 0LL);
    return Stats{v.front(), v[n / 2], sum / (long long)n, v[std::min(n - 1, n * 95 / 100)]};
}

// `warmup` untimed runs of f(), then `trials` timed ones.
template <class F> Stats measure(int warmup, int trials, F f) {
    for (int i = 0; i < warmup; i++) f();
    std::vector<long long> s;
    for (int i = 0; i < trials; i++) {
        long long t0 = now_ns();
        f();
        s.push_back(now_ns() - t0);
    }
    return stats(s);
}

inline const char* FLAGS = "-O3 -march=native -DNDEBUG";

inline std::string clean(std::string s) {
    for (auto& c : s) if (c == ',' || c == '\n') c = ';';
    return s;
}

// input = the throughput numerator (MiB for MB/s, items otherwise); unit "" = time only.
inline void emit(const char* slug, long long input, const char* lib, const std::string& ver, int warmup,
                 int trials, const Stats& s, bool ok, const std::string& notes, const char* unit) {
    std::string thr, u;
    if (unit && *unit && s.med > 0 && input >= 0) {
        char b[64];
        std::snprintf(b, sizeof b, "%.3f", (double)input * 1e9 / (double)s.med);
        thr = b;
        u = unit;
    }
    std::string nt = clean(notes) + (smoke() ? "; smoke" : "");
    std::printf("1,%s,%s,%s,io,,-1,,%lld,cpp,%s,%s,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%s,%s,%lld,-1,-1,-1,-1,%s,%s,%s,\n",
                env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug, input,
                env("PROFILE_LANG_VERSION", "").c_str(), lib, ver.c_str(), FLAGS, warmup, trials, s.mn, s.med,
                s.mean, s.p95, thr.c_str(), u.c_str(), peak_rss_kb(), ok ? "ok" : "invalid", ok ? "true" : "false",
                nt.c_str());
    std::fflush(stdout);
}

inline void skip(const char* slug, const char* lib, const std::string& reason) {
    std::printf("1,%s,%s,%s,io,,-1,,-1,cpp,%s,%s,,%s,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,\n",
                env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug,
                env("PROFILE_LANG_VERSION", "").c_str(), lib, FLAGS, clean(reason).c_str());
    std::fflush(stdout);
}

}  // namespace row
