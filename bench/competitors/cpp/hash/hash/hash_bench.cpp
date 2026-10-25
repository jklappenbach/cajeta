// C++ hash competitors (benchmark-reference unit 8): the hash/ ids of
// bench/hash/XXHash3.md and SipHash.md with the reference C implementations,
// same inputs, folds and checks as bench/src/main/cajeta/bench/hash/:
//   xxhash3 / xxhash3_128 / siphash: 1e7 hashes of a 32-byte key whose first 8
//     bytes are i little-endian (byte j = (j*31+7) & 0xFF);
//   xxhash3-256k / xxhash3_128-256k: 1e4 hashes of a 256 KB buffer, same rule.
// XXH3 is xxHash (third_party/xxhash, XXH_INLINE_ALL, the vendor/xxhash.lock
// commit); SipHash-2-4 is the Aumasson-Bernstein reference siphash.c
// (third_party/siphash, pins.tsv), compiled with the same flags.
#define XXH_INLINE_ALL
#include "xxhash.h"
extern "C" {
#include "siphash.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <new>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#define XXH_STR2(x) #x
#define XXH_STR(x) XXH_STR2(x)
#define XXH_VER_STRING XXH_STR(XXH_VERSION_MAJOR) "." XXH_STR(XXH_VERSION_MINOR) "." XXH_STR(XXH_VERSION_RELEASE)

using clk = std::chrono::steady_clock;
using U2 = std::pair<uint64_t, uint64_t>;
static const U2 XXH3_REF{0x15ae4a024a0e82deULL, 0}, XXH3_128_REF{0x7f4adb2d9f387c7fULL, 0xb318fb3ec50537efULL};
static const U2 XXH3_256K_REF{0x4f52c915522f9fe0ULL, 0}, XXH3_128_256K_REF{0x4f52c915522f9fe0ULL, 0x3c843c6da63e2dc3ULL};
static const U2 SIP_REF{0x1a6999cca3276432ULL, 0};
static const uint64_t N_KEYS = 10000000, N_BIG = 10000;
static const size_t BIG = 262144;

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
struct Cfg { std::string run_id, ts, ver; int warmup, trials; };

static void measure(const Cfg& c, const char* bench, unsigned long long bytes, const char* lib, const char* libver,
                    const char* notes, const std::function<U2()>& f, U2 want) {
    for (int w = 0; w < c.warmup; w++) sink(f());
    std::vector<long long> s; bool ok = true;
    for (int t = 0; t < c.trials; t++) {
        auto t0 = clk::now(); U2 r = f(); auto t1 = clk::now();
        sink(r); ok = ok && r == want;
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    ALLOCED = 0; sink(f()); unsigned long long a = ALLOCED.load();
    std::sort(s.begin(), s.end()); size_t k = s.size();
    long long mn = s[0], med = s[k / 2], mean = std::accumulate(s.begin(), s.end(), 0LL) / (long long)k, p95 = s[std::min(k - 1, k * 95 / 100)];
    double mbps = med > 0 ? (double)bytes / (double)med * 1e9 / 1048576.0 : 0.0;
    std::printf("1,%s,%s,%s,hash,,%llu,,%llu,cpp,%s,%s,%s,-O3 -march=native,%d,%d,%lld,%lld,%lld,%lld,%.1f,MB/s,%lld,-1,%llu,-1,-1,%s,%s,%s,\n",
        c.run_id.c_str(), c.ts.c_str(), bench, bytes, bytes, c.ver.c_str(), lib, libver, c.warmup, c.trials,
        mn, med, mean, p95, mbps, peak_rss_kb(), a, ok ? "ok" : "invalid", ok ? "true" : "false", notes);
}

static std::vector<uint8_t> buf(size_t n) { std::vector<uint8_t> b(n); for (size_t j = 0; j < n; j++) b[j] = (uint8_t)((j * 31 + 7) & 0xFF); return b; }

int main() {
    Cfg c{env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""),
          std::atoi(env("PROFILE_WARMUP", "3").c_str()), std::atoi(env("PROFILE_TRIALS", "10").c_str())};
    std::vector<uint8_t> key = buf(32), big = buf(BIG);
    const char* xv = XXH_VER_STRING;

    measure(c, "xxhash3", N_KEYS * 32, "xxHash", xv, "XXH3_64bits", [&] {
        uint64_t a = 0;
        for (uint64_t i = 0; i < N_KEYS; i++) { std::memcpy(key.data(), &i, 8); sink(key.data()); a ^= XXH3_64bits(key.data(), 32); }
        return U2{a, 0};
    }, XXH3_REF);
    measure(c, "xxhash3_128", N_KEYS * 32, "xxHash", xv, "XXH3_128bits", [&] {
        uint64_t lo = 0, hi = 0;
        for (uint64_t i = 0; i < N_KEYS; i++) {
            std::memcpy(key.data(), &i, 8); sink(key.data());
            XXH128_hash_t h = XXH3_128bits(key.data(), 32); lo ^= h.low64; hi ^= h.high64;
        }
        return U2{lo, hi};
    }, XXH3_128_REF);
    measure(c, "xxhash3-256k", N_BIG * BIG, "xxHash", xv, "XXH3_64bits", [&] {
        uint64_t a = 0;
        for (uint64_t i = 0; i < N_BIG; i++) { std::memcpy(big.data(), &i, 8); sink(big.data()); a ^= XXH3_64bits(big.data(), BIG); }
        return U2{a, 0};
    }, XXH3_256K_REF);
    measure(c, "xxhash3_128-256k", N_BIG * BIG, "xxHash", xv, "XXH3_128bits", [&] {
        uint64_t lo = 0, hi = 0;
        for (uint64_t i = 0; i < N_BIG; i++) {
            std::memcpy(big.data(), &i, 8); sink(big.data());
            XXH128_hash_t h = XXH3_128bits(big.data(), BIG); lo ^= h.low64; hi ^= h.high64;
        }
        return U2{lo, hi};
    }, XXH3_128_256K_REF);

    uint8_t k16[16]; for (int j = 0; j < 16; j++) k16[j] = (uint8_t)j;
    uint8_t vin[15]; for (int j = 0; j < 15; j++) vin[j] = (uint8_t)j;
    uint64_t vout = 0; siphash(vin, 15, k16, (uint8_t*)&vout, 8);
    const bool vec_ok = vout == 0xa129ca6149be45e5ULL;
    measure(c, "siphash", N_KEYS * 32, "siphash-reference", "veorq/SipHash@32d0676", "siphash() cROUNDS=2 dROUNDS=4 (key 00..0f)", [&] {
        uint64_t a = 0, h = 0;
        for (uint64_t i = 0; i < N_KEYS; i++) {
            std::memcpy(key.data(), &i, 8); sink(key.data());
            siphash(key.data(), 32, k16, (uint8_t*)&h, 8); a ^= h;
        }
        return U2{vec_ok ? a : 0, 0};
    }, SIP_REF);
    return 0;
}
