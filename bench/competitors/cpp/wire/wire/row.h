// Row emitter for the C++ wire/ competitor (a copy of codec/basen/row.h with the
// area and throughput unit as parameters). Prints v1 rows (31 columns,
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

inline const char* AREA = "wire";

// unit "MB/s": input_size is bytes, throughput MiB/s; "records/s": input_size records.
inline void emit(const char* slug, long long input, const char* lib, const std::string& ver, int warmup,
                 int trials, const Stats& s, bool ok, const std::string& notes, const std::string& variant,
                 const char* unit) {
    std::string n = notes;
    for (char& ch : n) if (ch == ',') ch = ';';
    std::string thr;
    if (s.med > 0) {
        char b[64];
        double per_s = (double)input / (double)s.med * 1e9;
        if (std::string(unit) == "MB/s") per_s /= 1048576.0;
        std::snprintf(b, sizeof b, "%.1f", per_s);
        thr = b;
    }
    std::printf("1,%s,%s,%s,%s,,-1,,%lld,cpp,%s,%s,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%s,%s,%lld,-1,-1,-1,-1,%s,%s,%s,%s\n",
                env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug, AREA,
                input, env("PROFILE_LANG_VERSION", "").c_str(), lib, ver.c_str(),
                FLAGS, warmup, trials, s.mn, s.med, s.mean, s.p95, thr.c_str(), unit, peak_rss_kb(),
                ok ? "ok" : "invalid", ok ? "true" : "false", n.c_str(), variant.c_str());
    std::fflush(stdout);
}

}  // namespace row
