// Shared row emitter for the C++ codec competitors (one copy per program
// directory so each program builds on its own). Prints v1 rows (31 columns,
// competitors/columns.txt order); lib/rows.sh's rows_v2 lifts them to v2.
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
inline int env_int(const char* k, int d) { return std::atoi(env(k, std::to_string(d).c_str()).c_str()); }

inline long long peak_rss_kb() {
    std::ifstream s("/proc/self/status");
    std::string line;
    while (std::getline(s, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long long kb = -1;
            std::sscanf(line.c_str(), "VmHWM: %lld", &kb);
            return kb;
        }
    }
    return -1;
}

inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

using clk = std::chrono::steady_clock;
inline long long ns_since(clk::time_point t0) {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
}

struct Stats { long long mn = -1, med = -1, mean = -1, p95 = -1; };
inline Stats stats(std::vector<long long> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    long long sum = std::accumulate(v.begin(), v.end(), 0LL);
    return Stats{v.front(), v[n / 2], sum / (long long)n, v[std::min(n - 1, n * 95 / 100)]};
}

// Time `trials` runs of f() after `warmup` untimed ones.
template <class F> Stats measure(int warmup, int trials, F f) {
    for (int i = 0; i < warmup; i++) f();
    std::vector<long long> s;
    s.reserve(trials);
    for (int i = 0; i < trials; i++) {
        auto t0 = clk::now();
        f();
        s.push_back(ns_since(t0));
    }
    return stats(s);
}

inline const char* FLAGS = "-O3 -march=native -DNDEBUG";

// throughput=true writes MB/s from input_size / median.
inline void emit(const char* slug, const std::string& dataset, long long dbytes, long long input,
                 const char* lib, const std::string& ver, int warmup, int trials, const Stats& s,
                 bool ok, const std::string& notes, const std::string& variant, bool throughput = true) {
    std::string thr, unit;
    if (throughput && s.med > 0) {
        char b[64];
        std::snprintf(b, sizeof b, "%.1f", (double)input / (double)s.med * 1e9 / 1048576.0);
        thr = b;
        unit = "MB/s";
    }
    std::printf("1,%s,%s,%s,codec,%s,%lld,,%lld,cpp,%s,%s,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%s,%s,%lld,-1,-1,-1,-1,%s,%s,%s,%s\n",
                env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug,
                dataset.c_str(), dbytes, input, env("PROFILE_LANG_VERSION", "").c_str(), lib, ver.c_str(),
                FLAGS, warmup, trials, s.mn, s.med, s.mean, s.p95, thr.c_str(), unit.c_str(), peak_rss_kb(),
                ok ? "ok" : "invalid", ok ? "true" : "false", notes.c_str(), variant.c_str());
    std::fflush(stdout);
}

inline void skip(const char* slug, const std::string& dataset, long long dbytes, long long input,
                 const char* lib, const std::string& ver, const std::string& reason, const std::string& variant) {
    std::printf("1,%s,%s,%s,codec,%s,%lld,,%lld,cpp,%s,%s,%s,%s,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,%s\n",
                env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug,
                dataset.c_str(), dbytes, input, env("PROFILE_LANG_VERSION", "").c_str(), lib, ver.c_str(),
                FLAGS, reason.c_str(), variant.c_str());
    std::fflush(stdout);
}

}  // namespace row
