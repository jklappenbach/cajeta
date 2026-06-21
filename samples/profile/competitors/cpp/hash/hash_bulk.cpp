// C++ hash competitors — bulk throughput over a deterministic 1 MiB buffer
// (buf[i] = i & 0xFF), the SMHasher bulk metric. One schema-conformant CSV row
// per algorithm (xxhash3 via the vendored xxHash header; sha256 + md5 via
// OpenSSL libcrypto). Cross-check: the digest matches the cross-language
// reference. Built directly: g++ -O3 -march=native ... -lcrypto.
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <openssl/md5.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

// xxHash exposes its version as integer macros — stringize them for the row.
#define XXH_STR2(x) #x
#define XXH_STR(x) XXH_STR2(x)
#define XXH_VER_STRING XXH_STR(XXH_VERSION_MAJOR) "." XXH_STR(XXH_VERSION_MINOR) "." XXH_STR(XXH_VERSION_RELEASE)

using clk = std::chrono::steady_clock;
static const size_t N = 1048576;
static const char* SHA256_REF = "fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83";
static const char* MD5_REF = "c35cc7d8d91728a0cb052831bc4ef372";
static const uint64_t XXH3_REF = 0xd36c0e13a3df139eULL;

static std::string env(const char* k, const char* d) {
    const char* v = std::getenv(k); return v ? v : d;
}
#include <atomic>
#include <cstdlib>
#include <new>
static std::atomic<unsigned long long> _ALLOCED{0};
static std::atomic<unsigned long long> _LASTALLOC{0};
void* operator new(std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
template <class F> static void _alloc_of(F run) { _ALLOCED = 0; run(); _LASTALLOC = _ALLOCED.load(); }
static unsigned long long _la() { return _LASTALLOC.load(); }

static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}
static std::string tohex(const unsigned char* b, int n) {
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (int i = 0; i < n; ++i) { s += h[b[i] >> 4]; s += h[b[i] & 0xF]; }
    return s;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 const char* lib, const char* ver, int warmup, int trials,
                 std::vector<long long> s, bool check_ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n,
              p95 = s[std::min(n - 1, n * 95 / 100)];
    double mbps = med > 0 ? (double)N / (double)med * 1e9 / 1048576.0 : 0.0;
    std::printf(
        "1,%s,%s,%s,hash,,%zu,,%zu,cpp,%s,%s,%s,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.1f,MB/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, N, N, env("PROFILE_LANG_VERSION", "").c_str(),
        lib, ver, warmup, trials, mn, med, mean, p95, mbps, peak_rss_kb(), (unsigned long long)_la(),
        check_ok ? "ok" : "invalid", check_ok ? "true" : "false");
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    std::vector<unsigned char> buf(N);
    for (size_t i = 0; i < N; ++i) buf[i] = (unsigned char)(i & 0xFF);

    // xxhash3
    {
        volatile uint64_t sink = 0;
        for (int i = 0; i < warmup; ++i) sink = XXH3_64bits(buf.data(), N);
        std::vector<long long> s;
        for (int i = 0; i < trials; ++i) {
            auto t0 = clk::now(); uint64_t h = XXH3_64bits(buf.data(), N); auto t1 = clk::now();
            sink = h;
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
        bool ok = (uint64_t)XXH3_64bits(buf.data(), N) == XXH3_REF; (void)sink;
        emit(run_id, ts, "xxhash3", "xxHash", XXH_VER_STRING, warmup, trials, s, ok);
    }
    // sha256 (OpenSSL)
    {
        unsigned char out[SHA256_DIGEST_LENGTH];
        for (int i = 0; i < warmup; ++i) SHA256(buf.data(), N, out);
        std::vector<long long> s;
        for (int i = 0; i < trials; ++i) {
            auto t0 = clk::now(); SHA256(buf.data(), N, out); auto t1 = clk::now();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
        SHA256(buf.data(), N, out);
        emit(run_id, ts, "sha256", "openssl", OPENSSL_FULL_VERSION_STR, warmup, trials, s,
             tohex(out, SHA256_DIGEST_LENGTH) == SHA256_REF);
    }
    // md5 (OpenSSL)
    {
        unsigned char out[MD5_DIGEST_LENGTH];
        for (int i = 0; i < warmup; ++i) MD5(buf.data(), N, out);
        std::vector<long long> s;
        for (int i = 0; i < trials; ++i) {
            auto t0 = clk::now(); MD5(buf.data(), N, out); auto t1 = clk::now();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
        MD5(buf.data(), N, out);
        emit(run_id, ts, "md5", "openssl", OPENSSL_FULL_VERSION_STR, warmup, trials, s,
             tohex(out, MD5_DIGEST_LENGTH) == MD5_REF);
    }
    return 0;
}
