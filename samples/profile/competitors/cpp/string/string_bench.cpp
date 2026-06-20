// C++ string competitors — the same four workloads the Cajeta side runs over a
// ~360 KB ASCII text (base "the quick brown fox jumps over the lazy dog " × 8192):
// string-search (absent needle), string-replace (fox→feline), string-uppercase,
// string-build-concat (build 4000 chars). One schema-conformant CSV row per
// (benchmark, library). Stdlib std::string idioms (find / a find-loop replace /
// ASCII toupper / += builder).
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
static const char* BASE = "the quick brown fox jumps over the lazy dog ";
static const int REPEAT = 8192;
static const char* PIECE = "abcdefgh";
static const int BUILD_ITERS = 500;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 const char* lib, size_t bytes, int warmup, int trials,
                 std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double mbps = med > 0 ? (double)bytes / (double)med * 1e9 / 1048576.0 : 0.0;
    std::printf(
        "1,%s,%s,%s,string,,%zu,,%zu,cpp,%s,%s,std,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.1f,MB/s,%lld,-1,-1,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, bytes, bytes, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, warmup, trials, mn, med, mean, p95, mbps, peak_rss_kb(),
        ok ? "ok" : "invalid", ok ? "true" : "false");
}

template <class F, class C>
static std::vector<long long> bench(int warmup, int trials, F f, C check, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto r = f(); (void)r; }
    std::vector<long long> s; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto t0 = clk::now();
        auto r = f();
        auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = check(r);
    }
    return s;
}

static std::string replace_all(const std::string& text, const std::string& from, const std::string& to) {
    std::string out; out.reserve(text.size());
    size_t pos = 0, prev = 0;
    while ((pos = text.find(from, prev)) != std::string::npos) {
        out.append(text, prev, pos - prev);
        out += to;
        prev = pos + from.size();
    }
    out.append(text, prev, text.size() - prev);
    return out;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());

    std::string text;
    text.reserve(REPEAT * 44);
    for (int i = 0; i < REPEAT; ++i) text += BASE;
    size_t nbytes = text.size();
    std::string needle = "zzzzqx-not-present";
    bool ok;

    // search (std::string::find) — absent ⇒ npos
    auto s1 = bench(warmup, trials, [&]{ return text.find(needle); },
                    [&](size_t r){ return r == std::string::npos; }, ok);
    emit(run_id, ts, "string-search", "std::string::find", nbytes, warmup, trials, s1, ok);

    // replace (find-loop) — output grows
    auto s2 = bench(warmup, trials, [&]{ return replace_all(text, "fox", "feline"); },
                    [&](const std::string& r){ return r.size() > nbytes; }, ok);
    emit(run_id, ts, "string-replace", "std find-loop", nbytes, warmup, trials, s2, ok);

    // uppercase (ASCII toupper) — same length
    auto s3 = bench(warmup, trials, [&]{
        std::string r = text;
        for (char& c : r) c = (char)std::toupper((unsigned char)c);
        return r;
    }, [&](const std::string& r){ return r.size() == nbytes; }, ok);
    emit(run_id, ts, "string-uppercase", "ascii-toupper", nbytes, warmup, trials, s3, ok);

    // build-concat (std::string += builder) — build 4000 chars
    auto s4 = bench(warmup, trials, [&]{
        std::string b;
        for (int i = 0; i < BUILD_ITERS; ++i) b += PIECE;
        return b;
    }, [&](const std::string& r){ return r.size() == (size_t)BUILD_ITERS * 8; }, ok);
    emit(run_id, ts, "string-build-concat", "std::string +=", (size_t)BUILD_ITERS * 8,
         warmup, trials, s4, ok);
    return 0;
}
