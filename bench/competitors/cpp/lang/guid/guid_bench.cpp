// C++ competitor: unit-5 lang/guid benchmarks (contract: u5-competitor-spec.md).
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
#include <array>
#include <cstring>
#include <uuid/uuid.h>

static std::string libuuid_version() {
    std::string v;
    if (FILE* p = popen("dpkg-query -W -f='${Version}' libuuid1 2>/dev/null", "r")) {
        char buf[128]; size_t n = std::fread(buf, 1, sizeof buf - 1, p); buf[n] = 0; v = buf; pclose(p);
    }
    for (char& c : v) if (c == ',' || c == '\n') c = ';';
    return v;
}

static int64_t distinct(const std::vector<std::array<unsigned char, 16>>& ids) {
    std::vector<unsigned __int128> v(ids.size());
    for (size_t k = 0; k < ids.size(); ++k) std::memcpy(&v[k], ids[k].data(), 16);
    std::sort(v.begin(), v.end());
    return (int64_t)(std::unique(v.begin(), v.end()) - v.begin());
}

int main() {
    Ctx ctx; CTX = &ctx;
    const int64_t N = 1000000;
    const std::string ver = libuuid_version();

    {
        std::vector<std::array<unsigned char, 16>> ids(N);  // preallocated 128-bit values
        run("guid-generate", N, "libuuid", ver.c_str(), "uuid_generate_random into a preallocated array", "", [&] {
            for (int64_t k = 0; k < N; ++k) uuid_generate_random(ids[k].data());
            return &ids;
        }, [&](const std::vector<std::array<unsigned char, 16>>* p) {
            bool ok = distinct(*p) == N;  // untimed distinct-count check
            for (auto& x : ids) x.fill(0);
            return ok;
        });
    }

    if (ctx.selected("guid-parse") || ctx.selected("guid-format")) {
        std::vector<std::array<unsigned char, 16>> ids(N);
        for (int64_t k = 0; k < N; ++k) uuid_generate_random(ids[k].data());
        std::vector<std::array<char, 37>> strs(N);
        for (int64_t k = 0; k < N; ++k) uuid_unparse_lower(ids[k].data(), strs[k].data());

        std::vector<std::array<unsigned char, 16>> parsed(N);
        run("guid-parse", N, "libuuid", ver.c_str(), "uuid_parse", "", [&] {
            for (int64_t k = 0; k < N; ++k) uuid_parse(strs[k].data(), parsed[k].data());
            return &parsed;
        }, [&](const std::vector<std::array<unsigned char, 16>>* p) {
            int64_t eq = 0;
            for (int64_t k = 0; k < N; ++k) eq += std::memcmp((*p)[k].data(), ids[k].data(), 16) == 0;
            for (auto& x : parsed) x.fill(0);  // the next trial must parse again
            return eq == N;
        });

        std::vector<std::array<char, 37>> out(N);
        run("guid-format", N, "libuuid", ver.c_str(), "uuid_unparse_lower into a preallocated char[37] per uuid", "", [&] {
            for (int64_t k = 0; k < N; ++k) uuid_unparse_lower(ids[k].data(), out[k].data());
            return &out;
        }, [&](const std::vector<std::array<char, 37>>* o) {
            int64_t total = 0;
            for (auto& s : *o) total += (int64_t)std::strlen(s.data());
            for (auto& s : out) s.fill(0);
            return total == 36000000;
        });
    }
    return 0;
}
