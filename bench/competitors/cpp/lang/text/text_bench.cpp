// C++ competitor: unit-5 lang/text benchmarks (contract: u5-competitor-spec.md).
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
#include <charconv>
#include <cstring>
#include <string_view>
#include <utility>

struct CpResult { int64_t count; int64_t sum; };

int main() {
    Ctx ctx; CTX = &ctx;

    if (ctx.selected("string-codepoint-walk")) {
        const std::string base = "Hello, wörld! Καλημέρα κόσμε, こんにちは世界 \U0001F680✨ ";
        if (base.size() != 74) { std::fprintf(stderr, "base size %zu != 74\n", base.size()); return 1; }
        std::string text; for (int r = 0; r < 14170; ++r) text += base;
        run("string-codepoint-walk", (long long)text.size(), "hand-decoded UTF-8", "std",
            "std has no UTF-8 codepoint iterator", "", [&] {
            const unsigned char* p = (const unsigned char*)text.data();
            const size_t n = text.size();
            int64_t count = 0, sum = 0; size_t k = 0;
            while (k < n) {
                uint32_t c = p[k];
                if (c < 0x80) { k += 1; }
                else if (c < 0xE0) { c = ((c & 0x1F) << 6) | (p[k + 1] & 0x3F); k += 2; }
                else if (c < 0xF0) { c = ((c & 0x0F) << 12) | ((p[k + 1] & 0x3F) << 6) | (p[k + 2] & 0x3F); k += 3; }
                else { c = ((c & 0x07) << 18) | ((p[k + 1] & 0x3F) << 12) | ((p[k + 2] & 0x3F) << 6) | (p[k + 3] & 0x3F); k += 4; }
                ++count; sum += c;
            }
            return CpResult{count, sum};
        }, [](CpResult r) { return r.count == 580970 && r.sum == 3748489290LL; });
    }

    if (ctx.selected("string-byteat-scan")) {
        const std::string base = "the quick brown fox jumps over the lazy dog\n";
        std::string text; for (int r = 0; r < 23832; ++r) text += base;
        run("string-byteat-scan", (long long)text.size(), "string::operator[]", "std", "", "", [&] {
            int64_t c = 0; const size_t n = text.size();
            for (size_t k = 0; k < n; ++k) if (text[k] == '\n') ++c;
            return c;
        }, [](int64_t c) { return c == 23832; });
    }

    if (ctx.selected("string-split")) {
        std::string text;
        for (int r = 0; r < 18397; ++r) text += "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa,";
        text += "end";
        const std::string_view sv(text);
        run("string-split", 100LL * (long long)text.size(), "string_view find", "std",
            "std has no split: string_view find loop into a vector<string_view>", "", [&] {
            int64_t total = 0;
            for (int r = 0; r < 100; ++r) {
                std::vector<std::string_view> pieces;
                size_t start = 0;
                for (;;) {
                    size_t pos = sv.find(',', start);
                    if (pos == std::string_view::npos) { pieces.push_back(sv.substr(start)); break; }
                    pieces.push_back(sv.substr(start, pos - start));
                    start = pos + 1;
                }
                total += (int64_t)pieces.size();
            }
            return total;
        }, [](int64_t t) { return t == 18397100LL; });
    }

    if (ctx.selected("string-compare")) {
        std::vector<std::string> a(64), b(64);
        for (int k = 0; k < 64; ++k) {
            std::string s = "abcdefghijklmnopqrstuvwxyz0123";
            s.push_back((char)('0' + k / 10)); s.push_back((char)('0' + k % 10));
            a[k] = s;
            b[k] = std::string(s.data(), s.size());  // a fresh copy
            if (k & 1) b[k].back() = 'x';
        }
        run("string-compare", 10000000LL, "string operator==", "std", "", "", [&] {
            int64_t eq = 0;
            for (int64_t i = 0; i < 10000000LL; ++i) if (a[i & 63] == b[i & 63]) ++eq;
            return eq;
        }, [](int64_t e) { return e == 5000000; });
    }

    run("stringbuilder-append", 1000000, "string::append", "std", "", "", [&] {
        std::string sb;
        for (int i = 0; i < 1000000; ++i) sb.append("0123456789abcdef", 16);
        std::string out = std::move(sb);  // the final string; a move: std::string is its own builder
        return (int64_t)launder(out.size());
    }, [](int64_t n) { return n == 16000000; });

    run("stringbuilder-append-mixed", 1000000, "string::append + to_chars", "std", "", "", [&] {
        std::string sb; char buf[64];
        for (int32_t i = 0; i < 1000000; ++i) {
            int m = i % 3;
            if (m == 0) { auto r = std::to_chars(buf, buf + sizeof buf, i); sb.append(buf, r.ptr - buf); }
            else if (m == 1) { auto r = std::to_chars(buf, buf + sizeof buf, (double)(i % 1000) + 0.5); sb.append(buf, r.ptr - buf); }
            else sb.append("abc", 3);
        }
        std::string out = std::move(sb);
        return (int64_t)launder(out.size());
    }, [](int64_t n) { return n == 4592962; });

    if (ctx.selected("stringbuilder-tostring")) {
        std::string builder;
        for (int r = 0; r < 1048576; ++r) builder.append("0123456789abcdef", 16);
        run("stringbuilder-tostring", 100, "std::string copy", "std", "copies 16 MiB per materialization", "", [&] {
            int64_t total = 0;
            for (int r = 0; r < 100; ++r) {
                std::string s(builder);
                launder(s.data());  // the copy must really happen
                total += (int64_t)s.size();
            }
            return total;
        }, [](int64_t t) { return t == 1677721600LL; });
    }
    return 0;
}
