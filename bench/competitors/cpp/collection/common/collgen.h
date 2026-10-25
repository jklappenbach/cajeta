// The collection/ competitors' shared inputs and row format (C++ side).
// Mirrors bench/src/main/cajeta/bench/collection/CollGen.cajeta exactly: the
// same draws in the same order give the same keys, queries and checks in all
// six languages.
//   next(): s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s >> 33
//           (xorshift64 on uint64_t, seed 88172645463325252; a 31-bit draw)
//   key(i): three xorshift64 rounds of i ^ 0x5851F42D4C957F2D, as int64_t
// Include once per program: it defines the counting global operator new.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

static std::atomic<uint64_t> g_alloced{0};
void* operator new(std::size_t n) {
    g_alloced.fetch_add(n, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace collgen {

inline uint64_t xs(uint64_t x) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
inline int64_t key(int64_t i) { return (int64_t) xs(xs(xs((uint64_t) i ^ 0x5851F42D4C957F2DULL))); }

struct Gen {
    uint64_t s = 88172645463325252ULL;
    int32_t next() { s = xs(s); return (int32_t) (s >> 33); }
    std::vector<int32_t> indices(size_t count, int32_t range) {
        std::vector<int32_t> o(count);
        for (auto& x : o) x = next() % range;
        return o;
    }
};

inline std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v && *v ? v : d; }
inline long num(const char* k, long d) { const char* v = std::getenv(k); return v && *v ? std::atol(v) : d; }
inline int warmup() { return (int) num("PROFILE_WARMUP", 3); }
inline int trials() { return (int) num("PROFILE_TRIALS", 10); }
inline bool selected(const std::string& id) {
    std::istringstream s(env("PROFILE_BENCH", ""));
    std::string w;
    bool any = false;
    while (s >> w) { any = true; if (w == id) return true; }
    return !any;
}
inline long peak_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string l;
    while (std::getline(f, l)) if (l.rfind("VmHWM:", 0) == 0) return std::atol(l.c_str() + 6);
    return -1;
}

template <class T> inline void sink(T const& v) { asm volatile("" : : "g"(&v) : "memory"); }

struct Measured { std::vector<int64_t> samples; bool ok = true; uint64_t alloc = 0; };

// Warm up, time `trials` calls of f, check the last result (untimed), then one
// more call's allocated bytes (the counting operator new).
template <class F, class C> Measured bench(F f, C check) {
    for (int i = 0; i < warmup(); i++) { auto r = f(); sink(r); }
    Measured m;
    for (int i = 0; i < trials(); i++) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = f();
        auto t1 = std::chrono::steady_clock::now();
        m.samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        m.ok = check(r);
        sink(r);
    }
    uint64_t a0 = g_alloced.load();
    { auto r = f(); sink(r); }
    m.alloc = g_alloced.load() - a0;
    return m;
}

inline std::string clean(std::string s) { std::replace(s.begin(), s.end(), ',', ' '); return s; }

inline void row(const char* id, const char* lib, const char* ver, int64_t input, Measured m, const char* notes) {
    auto s = m.samples;
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    int64_t sum = 0;
    for (auto v : s) sum += v;
    int64_t med = s[n / 2];
    double mops = med > 0 ? (double) input / (double) med * 1e3 : 0.0;
    std::printf("1,%s,%s,%s,collection,,%lld,,%lld,cpp,%s,%s,%s,-O3 -march=native -std=c++20,%d,%zu,%lld,%lld,%lld,%lld,%.2f,Mop/s,%ld,-1,%llu,-1,-1,%s,%s,%s,\n",
        env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), id, (long long) input, (long long) input,
        env("PROFILE_LANG_VERSION", "").c_str(), clean(lib).c_str(), ver, warmup(), n, (long long) s[0], (long long) med,
        (long long) (sum / (int64_t) n), (long long) s[std::min(n - 1, n * 95 / 100)], mops, peak_rss_kb(),
        (unsigned long long) m.alloc, m.ok ? "ok" : "invalid", m.ok ? "true" : "false", clean(notes).c_str());
    std::fflush(stdout);
}

inline void skip(const char* id, const char* lib, int64_t input, const char* reason) {
    std::printf("1,%s,%s,%s,collection,,%lld,,%lld,cpp,%s,%s,,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,%s,\n",
        env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), id, (long long) input, (long long) input,
        env("PROFILE_LANG_VERSION", "").c_str(), lib, clean(reason).c_str());
    std::fflush(stdout);
}

}  // namespace collgen
