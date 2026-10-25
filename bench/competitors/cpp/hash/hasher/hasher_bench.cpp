// C++ hash/hash-struct competitor (benchmark-reference unit 8): hash 1e7
// three-field structs (id: int64 = k*7919, x: int32 = k, y: int32 = k ^ 0x5bd1;
// a pool of 4096 cycled) the way bench/hash/Hash.md names it:
// boost::hash_combine over std::hash of each field (Boost.ContainerHash
// 1.89, header-only, third_party/boost-* pinned in pins.tsv). Fold: acc ^= h;
// low += h & 0xFFFF. The hash is deterministic, but the Cajeta and Rust ones
// are process-seeded, so the check is the same for all: a second pass
// reproduces (acc, low), neighbouring keys hash apart, and an equal-content
// struct hashes equal.
#include <boost/container_hash/hash.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <new>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

struct Key3 { int64_t id; int32_t x; int32_t y; };
static std::size_t hash_key(const Key3& k) {
    std::size_t seed = 0;
    boost::hash_combine(seed, std::hash<int64_t>{}(k.id));
    boost::hash_combine(seed, std::hash<int32_t>{}(k.x));
    boost::hash_combine(seed, std::hash<int32_t>{}(k.y));
    return seed;
}

template <class T> static inline void sink(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }
static std::atomic<unsigned long long> ALLOCED{0};
void* operator new(std::size_t n) { ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v && *v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) { long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb; }
    return -1;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local"), ts = env("PROFILE_RUN_TS", ""), ver = env("PROFILE_LANG_VERSION", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str()), trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    const long long N = 10000000;
    std::vector<Key3> pool(4096);
    for (int k = 0; k < 4096; k++) pool[k] = Key3{(int64_t)k * 7919, k, k ^ 0x5bd1};
    bool ok = hash_key(pool[7]) == hash_key(Key3{7 * 7919, 7, 7 ^ 0x5bd1});
    for (int k = 1; k < 4096; k++) ok = ok && hash_key(pool[k]) != hash_key(pool[k - 1]);
    auto run = [&] {
        uint64_t a = 0, l = 0;
        for (long long i = 0; i < N; i++) {
            const Key3* p = &pool[i & 4095]; sink(p);
            uint64_t h = hash_key(*p); a ^= h; l += h & 0xFFFF;
        }
        return std::pair<uint64_t, uint64_t>{a, l};
    };
    auto first = run();
    ok = ok && first.second > 0;
    for (int w = 0; w < warmup; w++) sink(run());
    std::vector<long long> s;
    for (int t = 0; t < trials; t++) {
        auto t0 = std::chrono::steady_clock::now(); auto r = run(); auto t1 = std::chrono::steady_clock::now();
        sink(r); ok = ok && r == first;
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    ALLOCED = 0; sink(run()); unsigned long long alloc = ALLOCED.load();
    std::sort(s.begin(), s.end()); size_t k = s.size();
    long long mn = s[0], med = s[k / 2], mean = std::accumulate(s.begin(), s.end(), 0LL) / (long long)k, p95 = s[std::min(k - 1, k * 95 / 100)];
    double mops = med > 0 ? (double)N / (double)med * 1e3 : 0.0;
    std::printf("1,%s,%s,hash-struct,hash,,%lld,,%lld,cpp,%s,boost::hash_combine,1.89.0,-O3 -march=native,%d,%d,%lld,%lld,%lld,%lld,%.2f,Mop/s,%lld,-1,%llu,-1,-1,%s,%s,boost::hash_combine over std::hash per field,\n",
        run_id.c_str(), ts.c_str(), N, N, ver.c_str(), warmup, trials, mn, med, mean, p95, mops, peak_rss_kb(), alloc,
        ok ? "ok" : "invalid", ok ? "true" : "false");
    return 0;
}
