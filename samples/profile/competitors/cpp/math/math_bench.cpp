// C++ math competitors — the same three scalar f64 kernels the Cajeta side runs:
// saxpy (z = 2.5*x + y over 1M), dot-product (sum x*y over 1M), matmul (200×200,
// A identity · B). Same init ⇒ the exact reference checksums match. Throughput in
// GFLOP/s. Scalar loops (no -ffast-math) isolate auto-vectorization on identical
// work; numpy (BLAS) is the Python competitor.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static const size_t N = 1000000;
static const int NDIM = 200;
static const double SAXPY_REF = 148250000.0;
static const double DOT_REF = 148499899.0;
static const double MATMUL_REF = 1980000.0;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
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
static bool approx(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(std::fabs(b), 1.0); }

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 size_t input, double flops, int warmup, int trials,
                 std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    double gflops = med > 0 ? flops / (double)med : 0.0;
    std::printf(
        "1,%s,%s,%s,math,,%zu,,%zu,cpp,%s,scalar,std,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.3f,GFLOP/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        warmup, trials, mn, med, mean, p95, gflops, peak_rss_kb(), (unsigned long long)_la(), ok ? "ok" : "invalid",
        ok ? "true" : "false");
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());

    std::vector<double> x(N), ys(N), yd(N);
    for (size_t i = 0; i < N; ++i) { x[i] = (double)(i % 100); ys[i] = (double)(i % 50); yd[i] = (double)(i % 7); }

    // saxpy
    {
        const double a = 2.5; std::vector<double> z(N); bool ok = true;
        for (int w = 0; w < warmup; ++w) for (size_t i = 0; i < N; ++i) z[i] = a * x[i] + ys[i];
        std::vector<long long> s;
        for (int t = 0; t < trials; ++t) {
            auto t0 = clk::now();
            double sum = 0.0;
            for (size_t i = 0; i < N; ++i) { z[i] = a * x[i] + ys[i]; sum += z[i]; }
            auto t1 = clk::now();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ok = approx(sum, SAXPY_REF);
        }
        emit(run_id, ts, "saxpy", N, 2.0 * (double)N, warmup, trials, s, ok);
    }
    // dot
    {
        bool ok = true;
        for (int w = 0; w < warmup; ++w) { double s = 0; for (size_t i = 0; i < N; ++i) s += x[i] * yd[i]; volatile double sink = s; (void)sink; }
        std::vector<long long> s;
        for (int t = 0; t < trials; ++t) {
            auto t0 = clk::now();
            double acc = 0.0;
            for (size_t i = 0; i < N; ++i) acc += x[i] * yd[i];
            auto t1 = clk::now();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ok = approx(acc, DOT_REF);
        }
        emit(run_id, ts, "dot-product", N, 2.0 * (double)N, warmup, trials, s, ok);
    }
    // matmul
    {
        std::vector<double> a(NDIM * NDIM), b(NDIM * NDIM), c(NDIM * NDIM);
        for (int i = 0; i < NDIM; ++i) for (int j = 0; j < NDIM; ++j) {
            int idx = i * NDIM + j; a[idx] = (i == j) ? 1.0 : 0.0; b[idx] = (double)(idx % 100);
        }
        auto mm = [&]() {
            for (int i = 0; i < NDIM; ++i) for (int j = 0; j < NDIM; ++j) {
                double acc = 0.0;
                for (int k = 0; k < NDIM; ++k) acc += a[i * NDIM + k] * b[k * NDIM + j];
                c[i * NDIM + j] = acc;
            }
        };
        bool ok = true;
        for (int w = 0; w < warmup; ++w) mm();
        std::vector<long long> s;
        for (int t = 0; t < trials; ++t) {
            auto t0 = clk::now(); mm(); auto t1 = clk::now();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ok = approx(std::accumulate(c.begin(), c.end(), 0.0), MATMUL_REF);
        }
        emit(run_id, ts, "matmul", NDIM * NDIM, 2.0 * std::pow((double)NDIM, 3), warmup, trials, s, ok);
    }
    return 0;
}
