// C++ math/ competitors (benchmark-reference unit 11, spec §6.8): every math/
// workload the pages name for C++, at the Cajeta side's sizes, data and checks
// (scratchpad WORKLOADS, mirrored in the bench/math pages):
//   Eigen 5.0.1   tensor-element-get/-set, tensor-elementwise/-broadcast/-reduce/
//                 -transpose/-cast, linalg-solve/-inverse/-qr/-svd, poly-fit,
//                 rotation-compose, transform-apply
//   FFTW 3.3.11   fft-1d, fft-2d (single precision fftwf, built by setup.sh)
//   pocketfft     fft-1d, fft-2d (header-only C++ pocketfft, float)
//   std           random-uniform/-normal (std::mt19937_64), poly-eval (Horner),
//                 ray-sphere, ray-aabb (plain structs)
// Prints v1 rows (31 columns); run.sh maps them to schema v2. Single-threaded
// (no -fopenmp). CAJETA_BENCH_SMOKE=1 selects the smoke sizes; MATH_IDS limits
// the ids. A thrown exception becomes a failed row with the message.
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <fftw3.h>
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
using std::string;
using std::vector;

static string env(const char* k, const char* d) { const char* v = std::getenv(k); return (v && *v) ? v : d; }
static const bool SMOKE = env("CAJETA_BENCH_SMOKE", "0") == "1" || env("CAJETA_BENCH_SMOKE", "0") == "true";
static const string RUN_ID = env("PROFILE_RUN_ID", "local");
static const string TS = env("PROFILE_RUN_TS", "");
static const int WARM = std::atoi(env("PROFILE_WARMUP", SMOKE ? "1" : "3").c_str());
static const int TRIALS = std::max(1, std::atoi(env("PROFILE_TRIALS", SMOKE ? "3" : "10").c_str()));
static const string LANGV = env("PROFILE_LANG_VERSION", "");
static const string EIGEN_V = std::to_string(EIGEN_WORLD_VERSION) == "3"
    ? std::to_string(EIGEN_MAJOR_VERSION) + "." + std::to_string(EIGEN_MINOR_VERSION) + "." + std::to_string(EIGEN_PATCH_VERSION)
    : "?";
static const string POCKET_V = "cpp@4d865cba";

static bool wanted(const char* id) {
    string ids = env("MATH_IDS", "");
    if (ids.empty()) return true;
    string pad = " " + ids + " ";
    return pad.find(string(" ") + id + " ") != string::npos;
}

static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status"); string l;
    while (std::getline(s, l)) if (l.rfind("VmHWM:", 0) == 0) {
        long long kb = -1; std::sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb;
    }
    return -1;
}

static const long long P = 10007;
static inline double u(long long i) { return (double)((i * 7919) % P) / 10007.0; }
static inline double v(long long i) { long long m = i % P; return (double)((m * m * 7 + i * 7919) % P) / 10007.0; }

static string smokeNote(const string& what) { return SMOKE ? ("; smoke size " + what) : string(""); }

static void row(const char* id, const string& variant, const string& lib, const string& libv, long long input,
                vector<long long> s, const string& status, const string& chk, string notes) {
    for (auto& c : notes) if (c == ',') c = ';';
    long long mn = -1, med = -1, mean = -1, p95 = -1;
    int w = 0, t = 0;
    if (!s.empty()) {
        std::sort(s.begin(), s.end());
        size_t n = s.size();
        mn = s.front(); med = s[n / 2]; mean = std::accumulate(s.begin(), s.end(), 0LL) / (long long)n;
        p95 = s[std::min(n - 1, n * 95 / 100)];
        w = WARM; t = TRIALS;
    }
    std::printf("1,%s,%s,%s,math,,%lld,,%lld,cpp,%s,%s,%s,-O3 -march=native,%d,%d,%lld,%lld,%lld,%lld,,,%lld,-1,-1,-1,-1,%s,%s,%s,%s\n",
                RUN_ID.c_str(), TS.c_str(), id, input, input, LANGV.c_str(), lib.c_str(), libv.c_str(), w, t,
                mn, med, mean, p95, peak_rss_kb(), status.c_str(), chk.c_str(), notes.c_str(), variant.c_str());
    std::fflush(stdout);
}

// Time `run` WARM + TRIALS times, then `check` once (after the last trial).
static void bench(const char* id, const string& variant, const string& lib, const string& libv, long long input,
                  const string& notes, const std::function<void()>& run, const std::function<bool()>& check) {
    try {
        for (int i = 0; i < WARM; ++i) run();
        vector<long long> s;
        for (int i = 0; i < TRIALS; ++i) {
            auto t0 = clk::now();
            run();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
        }
        bool ok = check();
        row(id, variant, lib, libv, input, s, ok ? "ok" : "invalid", ok ? "true" : "false", notes);
    } catch (const std::exception& e) {
        row(id, variant, lib, libv, input, {}, "failed", "", notes + "; threw: " + e.what());
    }
}

static bool relClose(double a, double ref, double tol) { return std::fabs(a - ref) <= tol * std::max(std::fabs(ref), 1e-300); }

using RowMatF = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

// ---------------------------------------------------------------- tensor
static void tensorBenches() {
    if (wanted("tensor-element-get")) {
        const long long n = 1000000;
        Eigen::VectorXd a(n);
        for (long long i = 0; i < n; ++i) a(i) = (double)i * 0.5;
        double s = 0; bool ok = true;
        bench("tensor-element-get", "flat-get", "eigen", EIGEN_V, n, "VectorXd::coeff per element; check: sum == 249999750000 exact",
              [&] { s = 0; for (long long i = 0; i < n; ++i) s += (double)(float)a.coeff(i); if (s != 249999750000.0) ok = false; },
              [&] { return ok; });
    }
    if (wanted("tensor-element-set")) {
        const long long n = 1000000;
        Eigen::VectorXd w = Eigen::VectorXd::Zero(n);
        double t = 0; bool ok = true;
        bench("tensor-element-set", "", "eigen", EIGEN_V, n, "VectorXd::coeffRef per element; check: w[n-1] == t+n-1 exact",
              [&] { t += 1.0; for (long long i = 0; i < n; ++i) w.coeffRef(i) = t + (double)i; if (w(n - 1) != t + (double)(n - 1)) ok = false; },
              [&] { return ok; });
    }
    if (wanted("tensor-elementwise")) {
        const long long n = SMOKE ? 1000000 : 10000000;
        Eigen::ArrayXf a(n), b(n), c(n), r;
        double ref = 0;
        for (long long i = 0; i < n; ++i) {
            a(i) = (float)u(i); b(i) = (float)u(i + 1); c(i) = (float)u(i + 2);
            ref += (double)a(i) * (double)b(i) + (double)c(i);
        }
        bench("tensor-elementwise", "", "eigen", EIGEN_V, n, "ArrayXf a*b+c fresh result; check: rel err <= 1e-5" + smokeNote("n=1e6 in place of 1e7"),
              [&] { Eigen::ArrayXf t = a * b + c; r.swap(t); },
              [&] { return relClose(r.cast<double>().sum(), ref, 1e-5); });
    }
    const long long rows = SMOKE ? 1000 : 10000, cols = 1000;
    if (wanted("tensor-broadcast") || wanted("tensor-reduce")) {
        RowMatF M(rows, cols);
        for (long long r = 0; r < rows; ++r) for (long long c = 0; c < cols; ++c) M(r, c) = (float)u(r * cols + c);
        string sn = smokeNote("rows=1000 in place of 10000");
        if (wanted("tensor-broadcast")) {
            Eigen::RowVectorXf rowv(cols);
            for (long long c = 0; c < cols; ++c) rowv(c) = (float)u(c + 5);
            double ref = 0;
            for (long long r = 0; r < rows; ++r) for (long long c = 0; c < cols; ++c) ref += (double)M(r, c) + (double)rowv(c);
            RowMatF R;
            bench("tensor-broadcast", "", "eigen", EIGEN_V, rows * cols, "M.rowwise() + row fresh result; check: rel err <= 1e-5" + sn,
                  [&] { RowMatF t = M.rowwise() + rowv; R.swap(t); },
                  [&] { return relClose(R.cast<double>().sum(), ref, 1e-5); });
        }
        if (wanted("tensor-reduce")) {
            Eigen::MatrixXd Md = M.cast<double>();
            double ref = Md.colwise().sum().sum() + Md.rowwise().sum().sum() + Md.colwise().maxCoeff().sum()
                       + Md.rowwise().maxCoeff().sum() + Md.colwise().mean().sum() + Md.rowwise().mean().sum();
            Eigen::RowVectorXf s0, x0, m0; Eigen::VectorXf s1, x1, m1;
            bench("tensor-reduce", "", "eigen", EIGEN_V, rows * cols, "colwise/rowwise sum maxCoeff mean; check: rel err <= 1e-3" + sn,
                  [&] { s0 = M.colwise().sum(); s1 = M.rowwise().sum(); x0 = M.colwise().maxCoeff(); x1 = M.rowwise().maxCoeff();
                        m0 = M.colwise().mean(); m1 = M.rowwise().mean(); },
                  [&] { double cs = s0.cast<double>().sum() + s1.cast<double>().sum() + x0.cast<double>().sum() + x1.cast<double>().sum()
                                  + m0.cast<double>().sum() + m1.cast<double>().sum();
                        return relClose(cs, ref, 1e-3); });
        }
    }
    if (wanted("tensor-transpose")) {
        const long long N = SMOKE ? 1024 : 4096;
        RowMatF M(N, N), T;
        for (long long i = 0; i < N; ++i) for (long long j = 0; j < N; ++j) M(i, j) = (float)u(i * N + j);
        bench("tensor-transpose", "", "eigen", EIGEN_V, N * N, "M.transpose().eval() into a row-major copy; check: exact equality" + smokeNote("N=1024 in place of 4096"),
              [&] { RowMatF t = M.transpose().eval(); T.swap(t); },
              [&] { for (long long i = 0; i < N; ++i) for (long long j = 0; j < N; ++j) if (T(j, i) != M(i, j)) return false; return true; });
    }
    if (wanted("tensor-cast")) {
        const long long n = SMOKE ? 1000000 : 10000000;
        Eigen::ArrayXf x(n); Eigen::ArrayXd y; Eigen::ArrayXi z;
        long long ref = 0;
        for (long long i = 0; i < n; ++i) { x(i) = (float)(u(i) * 1000 - 500); ref += (long long)(int)(double)x(i); }
        bench("tensor-cast", "", "eigen", EIGEN_V, n, "cast<double>() then cast<int>() (truncation); check: int64 checksum exact" + smokeNote("n=1e6 in place of 1e7"),
              [&] { Eigen::ArrayXd ty = x.cast<double>(); Eigen::ArrayXi tz = ty.cast<int>(); y.swap(ty); z.swap(tz); },
              [&] { long long s = 0; for (long long i = 0; i < n; ++i) s += z(i); return s == ref; });
    }
}

// ---------------------------------------------------------------- linalg
static Eigen::MatrixXd linA(long long n) {
    Eigen::MatrixXd A(n, n);
    for (long long i = 0; i < n; ++i) for (long long j = 0; j < n; ++j) A(i, j) = u(i * n + j) - 0.5 + (i == j ? (double)n : 0.0);
    return A;
}

static void linalgBenches() {
    const char* vars[2] = {"512", "2048"};
    for (int vi = 0; vi < 2; ++vi) {
        const long long full = vi == 0 ? 512 : 2048;
        const long long nS = SMOKE ? (vi == 0 ? 128 : 256) : full;
        const long long nSvd = SMOKE ? (vi == 0 ? 64 : 128) : full;
        string var = vars[vi];
        if (wanted("linalg-solve")) {
            Eigen::MatrixXd A = linA(nS); Eigen::VectorXd b(nS), x;
            for (long long i = 0; i < nS; ++i) b(i) = u(i + 3);
            bench("linalg-solve", var, "eigen", EIGEN_V, nS, "partialPivLu().solve; check: |Ax-b|inf/(|A|inf|x|inf+|b|inf) <= 1e-12" + smokeNote("n=" + std::to_string(nS) + " in place of " + var),
                  [&] { x = A.partialPivLu().solve(b); },
                  [&] { double r = (A * x - b).lpNorm<Eigen::Infinity>();
                        double an = A.cwiseAbs().rowwise().sum().maxCoeff();
                        return r / (an * x.lpNorm<Eigen::Infinity>() + b.lpNorm<Eigen::Infinity>()) <= 1e-12; });
        }
        if (wanted("linalg-inverse")) {
            Eigen::MatrixXd A = linA(nS), Ai;
            bench("linalg-inverse", var, "eigen", EIGEN_V, nS, "inverse() (partial-pivot LU); check: max|A*Ai-I| <= 1e-10" + smokeNote("n=" + std::to_string(nS) + " in place of " + var),
                  [&] { Ai = A.inverse(); },
                  [&] { return (A * Ai - Eigen::MatrixXd::Identity(nS, nS)).cwiseAbs().maxCoeff() <= 1e-10; });
        }
        if (wanted("linalg-qr")) {
            Eigen::MatrixXd A = linA(nS), Q, R;
            bench("linalg-qr", var, "eigen", EIGEN_V, nS, "HouseholderQR with householderQ() materialized; check: max|QR-A|/max|A| <= 1e-12" + smokeNote("n=" + std::to_string(nS) + " in place of " + var),
                  [&] { Eigen::HouseholderQR<Eigen::MatrixXd> qr(A); Q = qr.householderQ(); R = qr.matrixQR().triangularView<Eigen::Upper>(); },
                  [&] { return (Q * R - A).cwiseAbs().maxCoeff() / A.cwiseAbs().maxCoeff() <= 1e-12; });
        }
        if (wanted("linalg-svd")) {
            Eigen::MatrixXd A = linA(nSvd), U, Vt; Eigen::VectorXd S;
            bench("linalg-svd", var, "eigen", EIGEN_V, nSvd, "BDCSVD ComputeFullU|ComputeFullV; check: max|U diag(S) Vt-A|/max|A| <= 1e-10" + smokeNote("n=" + std::to_string(nSvd) + " in place of " + var),
                  [&] { Eigen::BDCSVD<Eigen::MatrixXd, Eigen::ComputeFullU | Eigen::ComputeFullV> svd(A);
                        U = svd.matrixU(); S = svd.singularValues(); Vt = svd.matrixV().transpose(); },
                  [&] { return (U * S.asDiagonal() * Vt - A).cwiseAbs().maxCoeff() / A.cwiseAbs().maxCoeff() <= 1e-10; });
        }
    }
}

// ---------------------------------------------------------------- fft
using cf = std::complex<float>;
static vector<cf> signal(long long N) {
    vector<cf> x(N);
    for (long long k = 0; k < N; ++k) x[k] = cf((float)(u(2 * k) - 0.5), (float)(u(2 * k + 1) - 0.5));
    return x;
}
static double maxErr(const vector<cf>& a, const vector<cf>& b, double scale) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, (double)std::abs(std::complex<double>(a[i]) * scale - std::complex<double>(b[i])));
    return m;
}
// Direct O(N^2) DFT in double of the N=64 signal.
static vector<std::complex<double>> directDft(const vector<cf>& x) {
    long long N = (long long)x.size();
    vector<std::complex<double>> X(N);
    for (long long k = 0; k < N; ++k) {
        std::complex<double> s = 0;
        for (long long j = 0; j < N; ++j) s += std::complex<double>(x[j]) * std::polar(1.0, -2.0 * M_PI * (double)(k * j % N) / (double)N);
        X[k] = s;
    }
    return X;
}
static double dftErr(const vector<cf>& y, const vector<std::complex<double>>& X) {
    double m = 0;
    for (size_t i = 0; i < y.size(); ++i) m = std::max(m, std::abs(std::complex<double>(y[i]) - X[i]));
    return m;
}

static vector<cf> fftwRun(vector<cf> x, const vector<int>& dims, int sign) {
    vector<cf> out(x.size());
    fftwf_plan p = dims.size() == 1
        ? fftwf_plan_dft_1d(dims[0], (fftwf_complex*)x.data(), (fftwf_complex*)out.data(), sign, FFTW_ESTIMATE)
        : fftwf_plan_dft_2d(dims[0], dims[1], (fftwf_complex*)x.data(), (fftwf_complex*)out.data(), sign, FFTW_ESTIMATE);
    fftwf_execute(p); fftwf_destroy_plan(p);
    return out;
}
static vector<cf> pocketRun(const vector<cf>& x, const vector<size_t>& dims, bool fwd) {
    vector<cf> out(x.size());
    pocketfft::shape_t shape(dims.begin(), dims.end());
    pocketfft::stride_t st(shape.size());
    ptrdiff_t s = sizeof(cf);
    for (int i = (int)shape.size() - 1; i >= 0; --i) { st[i] = s; s *= (ptrdiff_t)shape[i]; }
    pocketfft::shape_t axes; for (size_t i = 0; i < shape.size(); ++i) axes.push_back(i);
    pocketfft::c2c(shape, st, st, axes, fwd, x.data(), out.data(), 1.0f);
    return out;
}

static void fftCase(const char* id, const string& var, const vector<int>& dims, const string& sn, bool doDft) {
    long long N = 1; for (int d : dims) N *= d;
    vector<cf> x = signal(N);
    vector<cf> x64 = signal(64);
    auto X64 = directDft(x64);
    string chk = doDft ? "check: N=64 direct DFT max abs err <= 1e-4 and round trip max abs err <= 1e-4" : "check: round trip max abs err <= 1e-4";
    // FFTW: plan with FFTW_MEASURE in setup (planning overwrites the arrays, so fill afterwards).
    {
        vector<cf> in(N), out(N);
        fftwf_plan p = dims.size() == 1
            ? fftwf_plan_dft_1d(dims[0], (fftwf_complex*)in.data(), (fftwf_complex*)out.data(), FFTW_FORWARD, FFTW_MEASURE)
            : fftwf_plan_dft_2d(dims[0], dims[1], (fftwf_complex*)in.data(), (fftwf_complex*)out.data(), FFTW_FORWARD, FFTW_MEASURE);
        in = x;
        bench(id, var, "fftw", fftwf_version, N, "fftwf (single precision) plan FFTW_MEASURE in setup; " + chk + sn,
              [&] { fftwf_execute(p); },
              [&] {
                  bool ok = true;
                  if (doDft) ok = dftErr(fftwRun(x64, {64}, FFTW_FORWARD), X64) <= 1e-4;
                  vector<cf> back = fftwRun(out, dims, FFTW_BACKWARD);
                  return ok && maxErr(back, x, 1.0 / (double)N) <= 1e-4;
              });
        fftwf_destroy_plan(p);
    }
    {
        vector<size_t> sd(dims.begin(), dims.end());
        vector<cf> out;
        bench(id, var, "pocketfft", POCKET_V, N, "pocketfft c2c float (plan cached by pocketfft); " + chk + sn,
              [&] { out = pocketRun(x, sd, true); },
              [&] {
                  bool ok = true;
                  if (doDft) ok = dftErr(pocketRun(x64, {64}, true), X64) <= 1e-4;
                  vector<cf> back = pocketRun(out, sd, false);
                  return ok && maxErr(back, x, 1.0 / (double)N) <= 1e-4;
              });
    }
}

static void fftBenches() {
    if (wanted("fft-1d")) {
        fftCase("fft-1d", "2^16", {65536}, "", true);
        fftCase("fft-1d", "2^20", {SMOKE ? 262144 : 1048576}, smokeNote("N=262144 in place of 2^20"), true);
    }
    if (wanted("fft-2d")) {
        int d = SMOKE ? 512 : 2048;
        fftCase("fft-2d", "", {d, d}, smokeNote("512x512 in place of 2048x2048"), false);
    }
}

// ---------------------------------------------------------------- random
static void randomBenches() {
    const long long n = SMOKE ? 10000000 : 100000000;
    string sn = smokeNote("n=1e7 in place of 1e8");
    auto moments = [&](const vector<double>& b, double& m, double& var) {
        double s = 0; for (double x : b) s += x; m = s / (double)b.size();
        double q = 0; for (double x : b) q += (x - m) * (x - m); var = q / (double)b.size();
    };
    if (wanted("random-uniform")) {
        vector<double> buf(n); std::mt19937_64 g(42); std::uniform_real_distribution<double> d(0.0, 1.0);
        bench("random-uniform", "", "std", "mt19937_64", n, "std::mt19937_64(42) + uniform_real_distribution<double> into a preallocated vector; check: |mean-0.5| <= 1e-3 and |var-1/12| <= 1e-3" + sn,
              [&] { for (long long i = 0; i < n; ++i) buf[i] = d(g); },
              [&] { double m, var; moments(buf, m, var); return std::fabs(m - 0.5) <= 1e-3 && std::fabs(var - 1.0 / 12.0) <= 1e-3; });
    }
    if (wanted("random-normal")) {
        vector<double> buf(n); std::mt19937_64 g(42); std::normal_distribution<double> d(0.0, 1.0);
        bench("random-normal", "", "std", "mt19937_64", n, "std::mt19937_64(42) + normal_distribution<double>; check: |mean| <= 1e-3 and |var-1| <= 2e-3" + sn,
              [&] { for (long long i = 0; i < n; ++i) buf[i] = d(g); },
              [&] { double m, var; moments(buf, m, var); return std::fabs(m) <= 1e-3 && std::fabs(var - 1.0) <= 2e-3; });
    }
}

// ---------------------------------------------------------------- poly
static const double C[9] = {0.5, -1.25, 2.0, -0.75, 1.5, -2.25, 0.25, 1.75, -1.0};
static inline double horner(double x) { double r = C[0]; for (int k = 1; k < 9; ++k) r = r * x + C[k]; return r; }

static void polyBenches() {
    if (wanted("poly-eval")) {
        const long long n = SMOKE ? 10000000 : 100000000;
        vector<double> x(n), y(n);
        for (long long i = 0; i < n; ++i) x[i] = (double)i / (double)n;
        double integ = 0; for (int k = 0; k < 9; ++k) integ += C[k] / (double)(8 - k + 1);
        double ref = (double)n * integ - (horner(1.0) - horner(0.0)) / 2.0;
        bench("poly-eval", "", "std", "horner", n, "plain Horner loop into an output array; check: rel err <= 1e-7 vs Euler-Maclaurin" + smokeNote("n=1e7 in place of 1e8"),
              [&] { for (long long i = 0; i < n; ++i) y[i] = horner(x[i]); },
              [&] { double s = 0; for (double t : y) s += t; return relClose(s, ref, 1e-7); });
    }
    if (wanted("poly-fit")) {
        const long long m = SMOKE ? 100000 : 1000000;
        const int fits = SMOKE ? 10 : 100;
        Eigen::VectorXd x(m), y(m), coef;
        for (long long i = 0; i < m; ++i) { x(i) = -1.0 + 2.0 * (double)i / (double)(m - 1); y(i) = horner(x(i)); }
        bench("poly-fit", "", "eigen", EIGEN_V, m, "Vandermonde m x 9 built per fit + householderQr().solve; " + std::to_string(fits) + " fits per trial; check: max|coef-c| <= 1e-6" + smokeNote("m=1e5 and 10 fits in place of 1e6 and 100"),
              [&] {
                  for (int f = 0; f < fits; ++f) {
                      Eigen::MatrixXd V(m, 9);
                      for (long long i = 0; i < m; ++i) { double p = 1.0; for (int k = 8; k >= 0; --k) { V(i, k) = p; p *= x(i); } }
                      coef = V.householderQr().solve(y);
                  }
              },
              [&] { double e = 0; for (int k = 0; k < 9; ++k) e = std::max(e, std::fabs(coef(k) - C[k])); return e <= 1e-6; });
    }
}

// ---------------------------------------------------------------- geometry
struct V3 { float x, y, z; };
static inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
struct RayS { V3 o, d; };
struct SphereS { V3 c; float r; };
struct BoxS { V3 mn, mx; };

static vector<RayS> makeRays(long long R) {
    vector<RayS> rs(R);
    for (long long i = 0; i < R; ++i) {
        double dx = v(3 * i + 2) * 0.2 - 0.1, dy = v(3 * i + 7) * 0.2 - 0.1, dz = 1.0;
        double l = std::sqrt(dx * dx + dy * dy + dz * dz);
        rs[i] = {{(float)(v(3 * i) * 2 - 1), (float)(v(3 * i + 1) * 2 - 1), -5.0f}, {(float)(dx / l), (float)(dy / l), (float)(dz / l)}};
    }
    return rs;
}

static inline bool hitSphere(const RayS& ray, const SphereS& s) {
    V3 oc = sub(ray.o, s.c);
    float b = dot(oc, ray.d);
    float cc = dot(oc, oc) - s.r * s.r;
    float disc = b * b - cc;
    return disc >= 0.0f && (b <= 0.0f || cc <= 0.0f);
}
static inline bool slab(float o, float d, float mn, float mx, float& t0, float& t1) {
    float inv = 1.0f / d, ta = (mn - o) * inv, tb = (mx - o) * inv;
    if (ta > tb) std::swap(ta, tb);
    if (ta > t0) t0 = ta;
    if (tb < t1) t1 = tb;
    return t0 <= t1;
}
static inline bool hitBox(const RayS& r, const BoxS& b) {
    float t0 = 0.0f, t1 = 1e8f;
    return slab(r.o.x, r.d.x, b.mn.x, b.mx.x, t0, t1) && slab(r.o.y, r.d.y, b.mn.y, b.mx.y, t0, t1)
        && slab(r.o.z, r.d.z, b.mn.z, b.mx.z, t0, t1);
}

static void geometryBenches() {
    const long long R = SMOKE ? 1000 : 10000, S = 10000, tests = R * S;
    string sn = smokeNote("R=1000 (1e7 tests) in place of 10000");
    if (wanted("ray-sphere") || wanted("ray-aabb")) {
        vector<RayS> rays = makeRays(R);
        vector<SphereS> sph(S); vector<BoxS> box(S);
        for (long long j = 0; j < S; ++j) {
            double mx = v(5 * j) * 4 - 2, my = v(5 * j + 1) * 4 - 2, mz = v(5 * j + 2) * 4, s = 0.1 + v(5 * j + 3) * 0.3;
            V3 c{(float)mx, (float)my, (float)mz};
            sph[j] = {c, (float)(0.05 + v(5 * j + 3) * 0.2)};
            box[j] = {c, {(float)(mx + s), (float)(my + s), (float)(mz + s)}};
        }
        if (wanted("ray-sphere")) {
            long long hits = 0, ref = SMOKE ? 51613 : 502607;
            bench("ray-sphere", "", "std", "c++17", tests, "plain structs; check: |hits-ref| <= 1e-5*tests (ref from a float64 reference)" + sn,
                  [&] { long long h = 0; for (long long i = 0; i < R; ++i) for (long long j = 0; j < S; ++j) h += hitSphere(rays[i], sph[j]); hits = h; },
                  [&] { return std::llabs(hits - ref) <= (long long)(1e-5 * (double)tests); });
        }
        if (wanted("ray-aabb")) {
            long long hits = 0, ref = SMOKE ? 49068 : 476570;
            bench("ray-aabb", "", "std", "c++17", tests, "plain structs slab test; check: |hits-ref| <= 1e-5*tests (ref from a float64 reference)" + sn,
                  [&] { long long h = 0; for (long long i = 0; i < R; ++i) for (long long j = 0; j < S; ++j) h += hitBox(rays[i], box[j]); hits = h; },
                  [&] { return std::llabs(hits - ref) <= (long long)(1e-5 * (double)tests); });
        }
    }
    if (wanted("rotation-compose")) {
        const long long n = SMOKE ? 10000000 : 100000000;
        const double ref = SMOKE ? 38600.96393095818 : 386090.57108821085;
        vector<Eigen::Quaternionf> Q(1024); vector<Eigen::Vector3f> V(1024);
        for (long long j = 0; j < 1024; ++j) {
            double ax = v(4 * j) - 0.5, ay = v(4 * j + 1) - 0.5, az = v(4 * j + 2) - 0.5, l = std::sqrt(ax * ax + ay * ay + az * az);
            double a = v(4 * j + 3) * 2 * M_PI, sh = std::sin(a / 2);
            Q[j] = Eigen::Quaternionf((float)std::cos(a / 2), (float)(ax / l * sh), (float)(ay / l * sh), (float)(az / l * sh));
            V[j] = Eigen::Vector3f((float)(v(3 * j + 11) - 0.5), (float)(v(3 * j + 12) - 0.5), (float)(v(3 * j + 13) - 0.5));
        }
        double sum = 0;
        bench("rotation-compose", "", "eigen", EIGEN_V, n, "Eigen Quaternionf product and q*v; check: |sum-ref| <= 1e-6*n" + smokeNote("n=1e7 in place of 1e8"),
              [&] { double s = 0;
                    for (long long i = 0; i < n; ++i) {
                        Eigen::Quaternionf q = Q[i & 1023] * Q[(i >> 10) & 1023];
                        Eigen::Vector3f w = q * V[i & 1023];
                        s += (double)w.x() + (double)w.y() + (double)w.z();
                    }
                    sum = s; },
              [&] { return std::fabs(sum - ref) <= 1e-6 * (double)n; });
    }
    if (wanted("transform-apply")) {
        const long long m = 1000000; const int passes = SMOKE ? 10 : 100;
        double ax = 1, ay = 2, az = 3, l = std::sqrt(14.0);
        Eigen::Quaternionf q((float)std::cos(0.35), (float)(ax / l * std::sin(0.35)), (float)(ay / l * std::sin(0.35)), (float)(az / l * std::sin(0.35)));
        Eigen::Affine3f T = Eigen::Translation3f(1.0f, -2.0f, 3.0f) * q * Eigen::Scaling(2.0f);
        Eigen::Matrix3Xf Pm(3, m), out(3, m);
        for (long long k = 0; k < m; ++k)
            Pm.col(k) = Eigen::Vector3f((float)(v(3 * k) * 10 - 5), (float)(v(3 * k + 1) * 10 - 5), (float)(v(3 * k + 2) * 10 - 5));
        bench("transform-apply", "", "eigen", EIGEN_V, m * passes, "Eigen Affine3f (4x4) applied to a 3xN point matrix; 1e8 point applications as 100 passes over 1e6 points; check: |cs-ref| <= 2e-5*m" + smokeNote("10 passes (1e7 points) in place of 100"),
              [&] { for (int p = 0; p < passes; ++p) out.noalias() = T * Pm; },
              [&] { double cs = 0; for (long long k = 0; k < m; ++k) cs += (double)out(0, k) + (double)out(1, k) + (double)out(2, k);
                    return std::fabs(cs - 1910391.7256280927) <= 2e-5 * (double)m; });
    }
}

int main() {
    tensorBenches();
    linalgBenches();
    fftBenches();
    randomBenches();
    polyBenches();
    geometryBenches();
    return 0;
}
