// C++ CLBG competitors — the same three Computer Language Benchmarks Game
// classics the Cajeta side runs, at the same sizes, verified against the
// canonical checksums: clbg-mandelbrot (800², in-set 254099),
// clbg-fannkuch-redux (N=10, 38 / 73196), clbg-spectral-norm (N=100, ≈1.274224).
// Pure scalar stdlib. One schema-conformant CSV row per benchmark.
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
static const int MANDEL_N = 800;
static const long long MANDEL_REF = 254099;
static const int FANN_N = 10;
static const int SPECTRAL_N = 100;
static const double SPECTRAL_REF = 1.274224;

static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); std::string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static void emit(const std::string& run_id, const std::string& ts, const char* bench,
                 int input, int warmup, int trials, std::vector<long long> s, bool ok) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long sum = std::accumulate(s.begin(), s.end(), 0LL);
    long long mn = s.front(), med = s[n / 2], mean = sum / (long long)n, p95 = s[std::min(n - 1, n * 95 / 100)];
    std::printf(
        "1,%s,%s,%s,clbg,,%d,,%d,cpp,%s,scalar,std,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,,,%lld,-1,-1,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), bench, input, input, env("PROFILE_LANG_VERSION", "").c_str(),
        warmup, trials, mn, med, mean, p95, peak_rss_kb(), ok ? "ok" : "invalid", ok ? "true" : "false");
}

static long long mandelbrot(int n) {
    long long count = 0;
    for (int py = 0; py < n; ++py) {
        double ci = 2.0 * py / n - 1.0;
        for (int px = 0; px < n; ++px) {
            double cr = 2.0 * px / n - 1.5;
            double zr = 0, zi = 0; bool member = true;
            for (int it = 0; it < 50; ++it) {
                double nzr = zr * zr - zi * zi + cr, nzi = 2.0 * zr * zi + ci;
                zr = nzr; zi = nzi;
                if (zr * zr + zi * zi > 4.0) { member = false; break; }
            }
            if (member) ++count;
        }
    }
    return count;
}

static void fannkuch(int n, int& max_flips, int& checksum) {
    std::vector<int> perm(n), perm1(n), count(n);
    for (int i = 0; i < n; ++i) perm1[i] = i;
    max_flips = 0; checksum = 0; int perm_count = 0, r = n;
    for (;;) {
        while (r != 1) { count[r - 1] = r; --r; }
        perm = perm1;
        int flips = 0, k = perm[0];
        while (k != 0) { std::reverse(perm.begin(), perm.begin() + k + 1); ++flips; k = perm[0]; }
        if (flips > max_flips) max_flips = flips;
        checksum += (perm_count % 2 == 0) ? flips : -flips;
        ++perm_count;
        for (;;) {
            if (r == n) return;
            int p0 = perm1[0];
            for (int i = 0; i < r; ++i) perm1[i] = perm1[i + 1];
            perm1[r] = p0;
            if (--count[r] > 0) break;
            ++r;
        }
    }
}

static double spectral_norm(int n) {
    auto A = [](int i, int j) { return 1.0 / (((i + j) * (i + j + 1) / 2 + i + 1)); };
    auto mulAv = [&](const std::vector<double>& v, std::vector<double>& out) {
        for (int i = 0; i < n; ++i) { double s = 0; for (int j = 0; j < n; ++j) s += A(i, j) * v[j]; out[i] = s; }
    };
    auto mulAtv = [&](const std::vector<double>& v, std::vector<double>& out) {
        for (int i = 0; i < n; ++i) { double s = 0; for (int j = 0; j < n; ++j) s += A(j, i) * v[j]; out[i] = s; }
    };
    std::vector<double> u(n, 1.0), v(n, 0.0), t(n, 0.0);
    for (int it = 0; it < 10; ++it) {
        mulAv(u, t); mulAtv(t, v); mulAv(v, t); mulAtv(t, u);
    }
    double vbv = 0, vv = 0;
    for (int i = 0; i < n; ++i) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    return std::sqrt(vbv / vv);
}

template <class F, class C>
static std::vector<long long> bench(int warmup, int trials, F f, C check, bool& ok) {
    for (int i = 0; i < warmup; ++i) { auto r = f(); (void)r; }
    std::vector<long long> s; ok = true;
    for (int i = 0; i < trials; ++i) {
        auto t0 = clk::now(); auto r = f(); auto t1 = clk::now();
        s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ok = check(r);
    }
    return s;
}

int main() {
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());
    bool ok;

    auto s1 = bench(warmup, trials, []{ return mandelbrot(MANDEL_N); },
                    [&](long long r){ return r == MANDEL_REF; }, ok);
    emit(run_id, ts, "clbg-mandelbrot", MANDEL_N * MANDEL_N, warmup, trials, s1, ok);

    auto s2 = bench(warmup, trials, []{ int mf, cs; fannkuch(FANN_N, mf, cs); return std::pair<int,int>(mf, cs); },
                    [&](std::pair<int,int> r){ return r.first == 38 && r.second == 73196; }, ok);
    emit(run_id, ts, "clbg-fannkuch-redux", FANN_N, warmup, trials, s2, ok);

    auto s3 = bench(warmup, trials, []{ return spectral_norm(SPECTRAL_N); },
                    [&](double r){ return std::fabs(r - SPECTRAL_REF) < 1e-3; }, ok);
    emit(run_id, ts, "clbg-spectral-norm", SPECTRAL_N, warmup, trials, s3, ok);
    return 0;
}
