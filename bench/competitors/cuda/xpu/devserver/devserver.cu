// devserver — the CUDA C++ reference for every xpu/ and gfx/ catalog row
// (benchmark-reference unit 14, spec §6.13, §8.3). One workload per catalog id,
// the same sizes, inputs and checks as the Cajeta benchmark of that id
// (bench/src/main/cajeta/bench/xpu/, gfx/), written the way a CUDA programmer
// writes it: the vendor library where the page names one (cuBLAS for GEMM, CUB
// for reduce and scan, texture objects for sampling), plain kernels elsewhere.
//
//   devserver --rows [id ...]   measure each id's variants: warmup, trials (the
//                               host clock around one synchronized iteration,
//                               as the Cajeta harness times ours), then the
//                               check; one v1 row per variant on stdout
//                               (competitors/lib/rows.sh makes them v2).
//   devserver --serve           the interleaved-round competitor of the Cajeta
//                               harness (bench/xpu/XpuCompetitor.cajeta): one
//                               request per line on stdin --
//                                 prepare <id> <variant> -> ok <lang>|<platform>|<version>
//                                 run                    -> ok
//                                 check                  -> ok 1 | ok 0
//                                 quit
//   devserver --list            the ids and variants, no device touch.
//
// Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP (3), PROFILE_TRIALS (20).
// The same source builds for HIP (gpu_compat.h maps the runtime, CUB to hipCUB,
// cuBLAS to hipBLAS) — not compiled on Phoenix, which has no ROCm.
#include "gpu_compat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CK(x) do { gpuError_t e_ = (x); if (e_ != gpuSuccess) { \
    std::fprintf(stderr, "devserver: %s failed: %s (%s:%d)\n", #x, gpuGetErrorString(e_), __FILE__, __LINE__); \
    std::exit(3); } } while (0)
#define CKB(x) do { gpublasStatus_t s_ = (x); if (s_ != GPUBLAS_STATUS_SUCCESS) { \
    std::fprintf(stderr, "devserver: %s failed: status %d\n", #x, (int) s_); std::exit(3); } } while (0)

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string toolkit_version() {
    int v = 0;
    gpuRuntimeGetVersion(&v);
    char b[64];
    std::snprintf(b, sizeof b, "%s %d.%d", GPU_NAME, v / 1000, (v % 1000) / 10);
    return b;
}

// ---- kernels ---------------------------------------------------------------

#define TBSIZE 1024
__global__ void bs_copy(const double* a, double* c, unsigned n) { unsigned i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) c[i] = a[i]; }
__global__ void bs_mul(double* b, const double* c, double s, unsigned n) { unsigned i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) b[i] = s * c[i]; }
__global__ void bs_add(const double* a, const double* b, double* c, unsigned n) { unsigned i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) c[i] = a[i] + b[i]; }
__global__ void bs_triad(double* a, const double* b, const double* c, double s, unsigned n) { unsigned i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) a[i] = b[i] + s * c[i]; }
__global__ void bs_dot(const double* a, const double* b, double* sum, unsigned n) {
    __shared__ double tb[TBSIZE];
    unsigned i = blockDim.x * blockIdx.x + threadIdx.x;
    const unsigned lid = threadIdx.x;
    tb[lid] = 0.0;
    for (; i < n; i += blockDim.x * gridDim.x) tb[lid] += a[i] * b[i];
    for (unsigned offset = blockDim.x / 2; offset > 0; offset /= 2) {
        __syncthreads();
        if (lid < offset) tb[lid] += tb[lid + offset];
    }
    if (lid == 0) sum[blockIdx.x] = tb[lid];
}

__global__ void saxpy_k(float* y, const float* x, float a, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) y[i] = a * x[i] + y[i];
}

__global__ void empty_k(int* sink, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) sink[i] = sink[i];
}

__global__ void mandel_k(int* out, unsigned n) {
    unsigned g = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned rowBytes = n / 8;
    if (g >= n * rowBytes) return;
    unsigned y = g / rowBytes, xb = g % rowBytes;
    double ci = 2.0 * (double) y / (double) n - 1.0;
    int bits = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        double cr = 2.0 * (double) (xb * 8 + bit) / (double) n - 1.5;
        double zr = 0, zi = 0, tr = 0, ti = 0;
        for (int k = 0; k < 50 && tr + ti <= 4.0; k++) {
            zi = 2.0 * zr * zi + ci; zr = tr - ti + cr; tr = zr * zr; ti = zi * zi;
        }
        bits = bits * 2 + (tr + ti <= 4.0 ? 1 : 0);
    }
    out[g] = bits;
}

// mixbench-cuda's benchmark_func<float, 256, 8, 4, ci, false>, verbatim in shape,
// with thread 0's sum also stored for the checksum (as the Cajeta row does).
template <unsigned CI>
__global__ void mix_k(float seed, float* g_data, float* out) {
    const int stride = 256;
    int idx = blockIdx.x * 256 * 8 + threadIdx.x;
    const int big_stride = gridDim.x * 256 * 8;
    float tmps[8];
    for (int k = 0; k < 4; k++) {
#pragma unroll
        for (int j = 0; j < 8; j++) {
            tmps[j] = g_data[idx + j * stride + k * big_stride];
#pragma unroll 128
            for (unsigned i = 0; i < CI; i++) tmps[j] = tmps[j] * tmps[j] + seed;
        }
        float sum = 0;
#pragma unroll
        for (int j = 0; j < 8; j += 2) sum = tmps[j] * tmps[j + 1] + sum;
        if (sum == -1.0f) g_data[idx + k * big_stride] = sum;
        if (idx == 0 && k == 0) out[0] = sum;
    }
}

__global__ void tex_k(gpuTextureObject_t tex, float* out, unsigned per) {
    unsigned g = blockIdx.x * blockDim.x + threadIdx.x;
    float acc = 0.f;
    for (unsigned k = 0; k < per; k++) {
        unsigned tx = (g + k * 977) % 4096, ty = (g / 16 + k * 131) % 4096;
        float4 c = tex2D<float4>(tex, ((float) tx + 0.5f) / 4096.f, ((float) ty + 0.5f) / 4096.f);
        acc += c.x + c.y + c.z + c.w;
    }
    out[g] = acc;
}

// ---- workloads ---------------------------------------------------------------

struct Workload {
    std::string id, lang = "cuda-c++", library, unit;
    int64_t input = -1;
    virtual ~Workload() {}
    virtual std::vector<std::string> variants() { return {""}; }
    virtual bool prepare(const std::string& v) = 0;    // false: unsupported variant
    virtual void run() = 0;                           // one synchronized iteration
    virtual bool check() = 0;
    virtual std::string note() { return ""; }
    virtual void finish() {}
};

template <class T> static T* dalloc(size_t n) { T* p = nullptr; CK(gpuMalloc((void**) &p, n * sizeof(T))); return p; }

struct BabelStream : Workload {
    int kind; unsigned n = 1u << 25; double s = 0.4; double *a = 0, *b = 0, *c = 0, *sums = 0; unsigned nb = 0; double dotv = 0;
    std::vector<double> h, hs;
    BabelStream(int k) : kind(k) {
        const char* names[] = {"babelstream-copy", "babelstream-mul", "babelstream-add", "babelstream-triad", "babelstream-dot"};
        id = names[k]; library = "BabelStream 5.0 kernels"; unit = "B/s";
        input = (int64_t) ((k == 2 || k == 3) ? 3 : 2) * 8 * n;
    }
    bool prepare(const std::string&) override {
        if (!a) {
            a = dalloc<double>(n); b = dalloc<double>(n); c = dalloc<double>(n);
            gpuDeviceProp_t p; CK(gpuGetDeviceProperties(&p, 0)); nb = 4 * p.multiProcessorCount;
            sums = dalloc<double>(nb); h.resize(n); hs.resize(nb);
        }
        std::fill(h.begin(), h.end(), 0.1); CK(gpuMemcpy(a, h.data(), n * 8, gpuMemcpyHostToDevice));
        std::fill(h.begin(), h.end(), 0.2); CK(gpuMemcpy(b, h.data(), n * 8, gpuMemcpyHostToDevice));
        std::fill(h.begin(), h.end(), 0.0); CK(gpuMemcpy(c, h.data(), n * 8, gpuMemcpyHostToDevice));
        unsigned g = (n + TBSIZE - 1) / TBSIZE;
        if (kind >= 1) bs_copy<<<g, TBSIZE>>>(a, c, n);
        if (kind >= 2) bs_mul<<<g, TBSIZE>>>(b, c, s, n);
        if (kind >= 3) bs_add<<<g, TBSIZE>>>(a, b, c, n);
        if (kind >= 4) bs_triad<<<g, TBSIZE>>>(a, b, c, s, n);
        CK(gpuDeviceSynchronize());
        return true;
    }
    void run() override {
        unsigned g = (n + TBSIZE - 1) / TBSIZE;
        switch (kind) {
            case 0: bs_copy<<<g, TBSIZE>>>(a, c, n); break;
            case 1: bs_mul<<<g, TBSIZE>>>(b, c, s, n); break;
            case 2: bs_add<<<g, TBSIZE>>>(a, b, c, n); break;
            case 3: bs_triad<<<g, TBSIZE>>>(a, b, c, s, n); break;
            default:
                bs_dot<<<nb, TBSIZE>>>(a, b, sums, n);
                CK(gpuMemcpy(hs.data(), sums, nb * 8, gpuMemcpyDeviceToHost));
                dotv = 0; for (double v : hs) dotv += v;
        }
        CK(gpuDeviceSynchronize());
    }
    bool check() override {
        double ea = 0.1, eb = 0.2, ec = 0.0;
        ec = ea; eb = s * ec; ec = ea + eb; ea = eb + s * ec;
        if (kind == 4) return std::fabs(dotv - ea * eb * n) <= 1e-8 * ea * eb * n;
        double* src = kind == 0 ? c : kind == 1 ? b : kind == 2 ? c : a;
        double exp = kind == 0 ? 0.1 : kind == 1 ? s * 0.1 : kind == 2 ? 0.1 + s * 0.1 : ea;
        CK(gpuMemcpy(h.data(), src, n * 8, gpuMemcpyDeviceToHost));
        for (double v : h) if (std::fabs(v - exp) > 1e-12) return false;
        return true;
    }
    void finish() override { gpuFree(a); gpuFree(b); gpuFree(c); gpuFree(sums); a = 0; }
};

struct Saxpy : Workload {
    unsigned n = 1u << 26; float *x = 0, *y = 0; int runs = 0; std::vector<float> hy;
    Saxpy() { id = "device-saxpy"; library = "CUDA runtime"; unit = "B/s"; input = 12LL * n; }
    bool prepare(const std::string&) override {
        if (!x) {
            x = dalloc<float>(n); y = dalloc<float>(n); hy.resize(n);
            std::vector<float> hx(n); for (unsigned i = 0; i < n; i++) hx[i] = (float) (i % 16);
            CK(gpuMemcpy(x, hx.data(), n * 4, gpuMemcpyHostToDevice));
        }
        std::fill(hy.begin(), hy.end(), 1.0f); CK(gpuMemcpy(y, hy.data(), n * 4, gpuMemcpyHostToDevice));
        runs = 0; return true;
    }
    void run() override { saxpy_k<<<(n + 255) / 256, 256>>>(y, x, 2.0f, n); CK(gpuDeviceSynchronize()); runs++; }
    bool check() override {
        CK(gpuMemcpy(hy.data(), y, n * 4, gpuMemcpyDeviceToHost));
        for (unsigned i = 0; i < n; i++) if (hy[i] != 1.0f + (float) (2 * runs) * (float) (i % 16)) return false;
        return runs > 0;
    }
    void finish() override { gpuFree(x); gpuFree(y); x = 0; }
};

struct Reduce : Workload {
    unsigned n = 1u << 26; float *x = 0, *out = 0; void* tmp = 0; size_t tmpBytes = 0; double want = 0; float got = 0;
    Reduce() { id = "wave-reduce"; library = "CUB DeviceReduce::Sum"; unit = "B/s"; input = 4LL * n; }
    bool prepare(const std::string&) override {
        if (!x) {
            std::vector<float> hx(n); want = 0;
            for (unsigned i = 0; i < n; i++) { hx[i] = (float) (i % 8) * 0.125f; want += hx[i]; }
            x = dalloc<float>(n); out = dalloc<float>(1);
            CK(gpuMemcpy(x, hx.data(), n * 4, gpuMemcpyHostToDevice));
            CK(gpucub::DeviceReduce::Sum(nullptr, tmpBytes, x, out, (int) n));
            CK(gpuMalloc(&tmp, tmpBytes));
        }
        return true;
    }
    void run() override { CK(gpucub::DeviceReduce::Sum(tmp, tmpBytes, x, out, (int) n)); CK(gpuDeviceSynchronize()); }
    bool check() override { CK(gpuMemcpy(&got, out, 4, gpuMemcpyDeviceToHost)); return std::fabs(got - want) <= 1e-5 * want; }
    void finish() override { gpuFree(x); gpuFree(out); gpuFree(tmp); x = 0; }
};

struct Scan : Workload {
    unsigned n = 1u << 26; int *x = 0, *out = 0; void* tmp = 0; size_t tmpBytes = 0; std::vector<int> ref, h;
    Scan() { id = "scan"; library = "CUB DeviceScan::InclusiveSum"; unit = "B/s"; input = 8LL * n; }
    bool prepare(const std::string&) override {
        if (!x) {
            std::vector<int> hx(n); ref.resize(n); h.resize(n); int run = 0;
            for (unsigned i = 0; i < n; i++) { hx[i] = (int) (i % 5) - 2; run += hx[i]; ref[i] = run; }
            x = dalloc<int>(n); out = dalloc<int>(n);
            CK(gpuMemcpy(x, hx.data(), n * 4, gpuMemcpyHostToDevice));
            CK(gpucub::DeviceScan::InclusiveSum(nullptr, tmpBytes, x, out, (int) n));
            CK(gpuMalloc(&tmp, tmpBytes));
        }
        return true;
    }
    void run() override { CK(gpucub::DeviceScan::InclusiveSum(tmp, tmpBytes, x, out, (int) n)); CK(gpuDeviceSynchronize()); }
    bool check() override { CK(gpuMemcpy(h.data(), out, n * 4, gpuMemcpyDeviceToHost)); return h == ref; }
    void finish() override { gpuFree(x); gpuFree(out); gpuFree(tmp); x = 0; }
};

// cuBLAS is column-major; a row-major C = A.B is the column-major C^T = B^T.A^T,
// so the call passes B then A. A = identity for f64 and f16 (as the Cajeta rows),
// small integers for f32 and bf16; the check is exact on 512 entries.
struct Gemm : Workload {
    int dt; int n = 4096; gpublasHandle_t hb = nullptr; void *a = 0, *b = 0, *c = 0;
    std::vector<double> ha, hbm; std::vector<double> hc;
    Gemm(int t) : dt(t) {   // 0 f64, 1 f32, 2 f16, 3 bf16
        const char* names[] = {"gemm-f64", "gemm-f32", "gemm-f16", "gemm-bf16"};
        id = names[t]; unit = "flop/s"; input = 2LL * n * n * n;
        library = t == 0 ? "cuBLAS cublasDgemm" : t == 1 ? "cuBLAS cublasSgemm" : t == 2 ? "cuBLAS cublasGemmEx f16 f32-acc" : "cuBLAS cublasGemmEx bf16 f32-acc";
    }
    std::vector<std::string> variants() override { return {"n4096"}; }
    size_t esz() { return dt == 0 ? 8 : dt == 1 ? 4 : 2; }
    bool prepare(const std::string&) override {
        if (a) return true;
        CKB(gpublasCreate(&hb));
        size_t m = (size_t) n * n; ha.resize(m); hbm.resize(m);
        bool ident = dt == 0 || dt == 2;
        for (size_t i = 0; i < m; i++) {
            if (ident) { ha[i] = (i / n == i % n) ? 1.0 : 0.0; hbm[i] = (double) (i % 100); }
            else { ha[i] = (double) ((i / 7) % 4); hbm[i] = (double) ((i / 3) % 5); }
        }
        a = dalloc<char>(m * esz()); b = dalloc<char>(m * esz()); c = dalloc<char>(m * (dt == 0 ? 8 : 4));
        upload(a, ha); upload(b, hbm);
        return true;
    }
    void upload(void* d, const std::vector<double>& v) {
        size_t m = v.size();
        if (dt == 0) { CK(gpuMemcpy(d, v.data(), m * 8, gpuMemcpyHostToDevice)); return; }
        if (dt == 1) { std::vector<float> f(v.begin(), v.end()); CK(gpuMemcpy(d, f.data(), m * 4, gpuMemcpyHostToDevice)); return; }
        if (dt == 2) { std::vector<gpu_half> f(m); for (size_t i = 0; i < m; i++) f[i] = gpu_float2half((float) v[i]); CK(gpuMemcpy(d, f.data(), m * 2, gpuMemcpyHostToDevice)); return; }
        std::vector<gpu_bf16> f(m); for (size_t i = 0; i < m; i++) f[i] = gpu_float2bf16((float) v[i]); CK(gpuMemcpy(d, f.data(), m * 2, gpuMemcpyHostToDevice));
    }
    void run() override {
        if (dt == 0) { double al = 1, be = 0; CKB(gpublasDgemm(hb, GPUBLAS_OP_N, GPUBLAS_OP_N, n, n, n, &al, (double*) b, n, (double*) a, n, &be, (double*) c, n)); }
        else if (dt == 1) { float al = 1, be = 0; CKB(gpublasSgemm(hb, GPUBLAS_OP_N, GPUBLAS_OP_N, n, n, n, &al, (float*) b, n, (float*) a, n, &be, (float*) c, n)); }
        else { float al = 1, be = 0; auto t = dt == 2 ? GPU_R_16F : GPU_R_16BF;
            CKB(gpublasGemmEx(hb, GPUBLAS_OP_N, GPUBLAS_OP_N, n, n, n, &al, b, t, n, a, t, n, &be, c, GPU_R_32F, n, GPUBLAS_COMPUTE_32F, GPUBLAS_GEMM_DEFAULT)); }
        CK(gpuDeviceSynchronize());
    }
    bool check() override {
        size_t m = (size_t) n * n; hc.resize(m);
        if (dt == 0) CK(gpuMemcpy(hc.data(), c, m * 8, gpuMemcpyDeviceToHost));
        else { std::vector<float> f(m); CK(gpuMemcpy(f.data(), c, m * 4, gpuMemcpyDeviceToHost)); for (size_t i = 0; i < m; i++) hc[i] = f[i]; }
        for (int s = 0; s < 512; s++) {
            int r = (s * 37 + 11) % n, cc = (s * 101 + 5) % n; double want = 0;
            for (int k = 0; k < n; k++) want += ha[(size_t) r * n + k] * hbm[(size_t) k * n + cc];
            if (hc[(size_t) r * n + cc] != want) return false;
        }
        return true;
    }
    void finish() override { gpuFree(a); gpuFree(b); gpuFree(c); gpublasDestroy(hb); a = 0; }
};

static int64_t host_mandel(unsigned n) {
    int64_t count = 0;
    for (unsigned y = 0; y < n; y++) {
        double ci = 2.0 * (double) y / (double) n - 1.0;
        for (unsigned x = 0; x < n; x++) {
            double cr = 2.0 * (double) x / (double) n - 1.5, zr = 0, zi = 0, tr = 0, ti = 0;
            for (int k = 0; k < 50 && tr + ti <= 4.0; k++) { zi = 2.0 * zr * zi + ci; zr = tr - ti + cr; tr = zr * zr; ti = zi * zi; }
            if (tr + ti <= 4.0) count++;
        }
    }
    return count;
}

struct Mandel : Workload {
    unsigned n = 16000; int* out = 0; int64_t want = 0, got = -1; std::vector<int> h;
    Mandel() { id = "mandelbrot-device"; library = "CUDA runtime"; unit = "pixels/s"; input = (int64_t) n * n; }
    bool prepare(const std::string&) override {
        if (!out) { out = dalloc<int>((size_t) n * (n / 8)); h.resize((size_t) n * (n / 8)); want = host_mandel(n); }
        return true;
    }
    void run() override { unsigned t = n * (n / 8); mandel_k<<<(t + 255) / 256, 256>>>(out, n); CK(gpuDeviceSynchronize()); }
    bool check() override {
        CK(gpuMemcpy(h.data(), out, h.size() * 4, gpuMemcpyDeviceToHost));
        got = 0; for (int v : h) got += __builtin_popcount((unsigned) v);
        return want > 0 && std::llabs(got - want) <= (int64_t) (1e-5 * want);
    }
    void finish() override { gpuFree(out); out = 0; }
};

struct Mix : Workload {
    unsigned n = 32u * 1024 * 1024, ci = 0; float *d = 0, *o = 0;
    Mix() { id = "mixbench-sweep"; library = "mixbench benchmark_func (float, 256, 8, 4)"; unit = "flop/s"; }
    std::vector<std::string> variants() override { return {"ci0", "ci1", "ci2", "ci4", "ci8", "ci16", "ci32", "ci64", "ci128", "ci256"}; }
    bool prepare(const std::string& v) override {
        if (!d) {
            d = dalloc<float>(n); o = dalloc<float>(1);
            std::vector<float> h(n, 0.5f); CK(gpuMemcpy(d, h.data(), n * 4, gpuMemcpyHostToDevice));
        }
        ci = (unsigned) std::atoi(v.c_str() + 2);
        int64_t t = n / 32; input = (8 * t + 16 * (int64_t) ci * t) * 4;
        float z = 0; CK(gpuMemcpy(o, &z, 4, gpuMemcpyHostToDevice));
        return true;
    }
    void run() override {
        unsigned grid = n / 8 / 4 / 256;
        switch (ci) {
            case 0: mix_k<0><<<grid, 256>>>(0.25f, d, o); break;
            case 1: mix_k<1><<<grid, 256>>>(0.25f, d, o); break;
            case 2: mix_k<2><<<grid, 256>>>(0.25f, d, o); break;
            case 4: mix_k<4><<<grid, 256>>>(0.25f, d, o); break;
            case 8: mix_k<8><<<grid, 256>>>(0.25f, d, o); break;
            case 16: mix_k<16><<<grid, 256>>>(0.25f, d, o); break;
            case 32: mix_k<32><<<grid, 256>>>(0.25f, d, o); break;
            case 64: mix_k<64><<<grid, 256>>>(0.25f, d, o); break;
            case 128: mix_k<128><<<grid, 256>>>(0.25f, d, o); break;
            default: mix_k<256><<<grid, 256>>>(0.25f, d, o); break;
        }
        CK(gpuDeviceSynchronize());
    }
    bool check() override { float v = 0; CK(gpuMemcpy(&v, o, 4, gpuMemcpyDeviceToHost)); return v == 1.0f; }
    void finish() override { gpuFree(d); gpuFree(o); d = 0; }
};

struct Launch : Workload {
    int* sink = 0; int64_t made = 0, runs = 0;
    Launch() { id = "launch-overhead"; library = "CUDA runtime <<<>>>"; unit = "launches/s"; input = 10000; }
    bool prepare(const std::string&) override { if (!sink) sink = dalloc<int>(32); made = runs = 0; return true; }
    void run() override { for (int i = 0; i < 10000; i++) { empty_k<<<1, 32>>>(sink, 0); made++; } CK(gpuDeviceSynchronize()); runs++; }
    bool check() override { return gpuGetLastError() == gpuSuccess && runs > 0 && made == runs * 10000; }
    void finish() override { gpuFree(sink); sink = 0; }
};

struct Transfer : Workload {
    size_t n = 1u << 28; float* d = 0; std::vector<float> src, dst; double want = 0;
    Transfer() { id = "host-device-transfer"; library = "cudaMemcpy (pageable)"; unit = "B/s"; input = 8LL * (int64_t) n; }
    bool prepare(const std::string&) override {
        if (!d) { d = dalloc<float>(n); src.resize(n); dst.resize(n); want = 0; for (size_t i = 0; i < n; i++) { src[i] = (float) (i % 1000); want += src[i]; } }
        return true;
    }
    void run() override { CK(gpuMemcpy(d, src.data(), n * 4, gpuMemcpyHostToDevice)); CK(gpuMemcpy(dst.data(), d, n * 4, gpuMemcpyDeviceToHost)); }
    bool check() override { std::fill(dst.begin(), dst.end(), 0.f); run(); double s = 0; for (float v : dst) s += v; return s == want; }
    void finish() override { gpuFree(d); d = 0; }
};

struct AllocFree : Workload {
    int64_t good = 0, runs = 0;
    AllocFree() { id = "kernelbuffer-alloc-free"; library = "cudaMalloc/cudaFree"; unit = "allocs/s"; input = 10000; }
    bool prepare(const std::string&) override { good = runs = 0; return true; }
    void run() override {
        for (int i = 0; i < 10000; i++) { void* p = nullptr; if (gpuMalloc(&p, 64u << 20) == gpuSuccess && p) good++; CK(gpuFree(p)); }
        runs++;
    }
    bool check() override { return runs > 0 && good == runs * 10000; }
};

struct TwoStream : Workload {
    unsigned n = 1u << 24; int chain = 16, runs = 0; bool two = false; float *x = 0, *ya = 0, *yb = 0; gpuStream_t s1, s2; std::vector<float> h;
    TwoStream() { id = "two-stream-overlap"; library = "CUDA streams"; unit = "B/s"; input = 2LL * 16 * 12 * n; }
    std::vector<std::string> variants() override { return {"one-stream", "two-streams"}; }
    bool prepare(const std::string& v) override {
        if (!x) {
            x = dalloc<float>(n); ya = dalloc<float>(n); yb = dalloc<float>(n); h.resize(n);
            for (unsigned i = 0; i < n; i++) h[i] = (float) (i % 16);
            CK(gpuMemcpy(x, h.data(), n * 4, gpuMemcpyHostToDevice));
            CK(gpuStreamCreateWithFlags(&s1, gpuStreamNonBlocking)); CK(gpuStreamCreateWithFlags(&s2, gpuStreamNonBlocking));
        }
        two = v == "two-streams";
        std::fill(h.begin(), h.end(), 1.0f);
        CK(gpuMemcpy(ya, h.data(), n * 4, gpuMemcpyHostToDevice)); CK(gpuMemcpy(yb, h.data(), n * 4, gpuMemcpyHostToDevice));
        runs = 0; return true;
    }
    void run() override {
        for (int i = 0; i < chain; i++) {
            saxpy_k<<<(n + 255) / 256, 256, 0, s1>>>(ya, x, 2.0f, n);
            saxpy_k<<<(n + 255) / 256, 256, 0, two ? s2 : s1>>>(yb, x, 2.0f, n);
        }
        CK(gpuStreamSynchronize(s1)); if (two) CK(gpuStreamSynchronize(s2));
        runs++;
    }
    bool check() override {
        float step = (float) (2 * chain * runs);
        for (float* y : {ya, yb}) {
            CK(gpuMemcpy(h.data(), y, n * 4, gpuMemcpyDeviceToHost));
            for (unsigned i = 0; i < n; i++) if (h[i] != 1.0f + step * (float) (i % 16)) return false;
        }
        return runs > 0;
    }
    void finish() override { gpuFree(x); gpuFree(ya); gpuFree(yb); gpuStreamDestroy(s1); gpuStreamDestroy(s2); x = 0; }
};

// CUDA events, uncalibrated: the driver's own tick-to-millisecond conversion.
struct Timer : Workload {
    int* sink = 0; float *bx = 0, *by = 0; gpuEvent_t e0, e1; double minMs = -1, sumMs = 0; int64_t reads = 0; double agree = 0;
    Timer() { id = "kerneltimer-resolution"; library = "CUDA events (cudaEventElapsedTime)"; unit = "brackets/s"; input = 1000; }
    bool prepare(const std::string&) override {
        if (!sink) { sink = dalloc<int>(32); bx = dalloc<float>(1u << 24); by = dalloc<float>(1u << 24); CK(gpuEventCreate(&e0)); CK(gpuEventCreate(&e1)); }
        minMs = -1; sumMs = 0; reads = 0; return true;
    }
    void run() override {
        for (int i = 0; i < 1000; i++) {
            CK(gpuEventRecord(e0, 0)); empty_k<<<1, 32>>>(sink, 0); CK(gpuEventRecord(e1, 0)); CK(gpuEventSynchronize(e1));
            float ms = 0; if (gpuEventElapsedTime(&ms, e0, e1) == gpuSuccess) { if (minMs < 0 || ms < minMs) minMs = ms; sumMs += ms; reads++; }
        }
    }
    bool check() override {
        int64_t h0 = now_ns(); CK(gpuEventRecord(e0, 0));
        for (int i = 0; i < 64; i++) saxpy_k<<<(1u << 24) / 256, 256>>>(by, bx, 2.0f, 1u << 24);
        CK(gpuEventRecord(e1, 0)); CK(gpuEventSynchronize(e1)); int64_t h1 = now_ns();
        float ms = 0; CK(gpuEventElapsedTime(&ms, e0, e1));
        agree = ms * 1e6 / (double) (h1 - h0);
        return reads > 0 && agree >= 0.99 && agree <= 1.01;
    }
    std::string note() override {
        char b[200]; std::snprintf(b, sizeof b, "min interval %.0f ns; mean %.0f ns; events/host on a busy bracket %.4f (uncalibrated)",
                                   minMs * 1e6, reads ? sumMs * 1e6 / reads : -1.0, agree);
        return b;
    }
    void finish() override { gpuFree(sink); gpuFree(bx); gpuFree(by); gpuEventDestroy(e0); gpuEventDestroy(e1); sink = 0; }
};

struct Texture : Workload {
    unsigned threads = 1u << 20, per = 256; gpuArray_t arr = 0; gpuTextureObject_t tex = 0; float* out = 0; double want = 0; std::vector<float> h;
    Texture() { id = "texture-sample-rate"; lang = "cuda-c++"; library = "CUDA texture objects (RGBA8 normalized, linear)"; unit = "samples/s"; input = (int64_t) threads * per; }
    static int texel(int x, int y, int ch) { return (x * 3 + y * 5 + ch * 7) % 256; }
    bool prepare(const std::string&) override {
        if (arr) return true;
        const int w = 4096; std::vector<uchar4> px((size_t) w * w);
        for (int y = 0; y < w; y++) for (int x = 0; x < w; x++)
            px[(size_t) y * w + x] = make_uchar4(texel(x, y, 0), texel(x, y, 1), texel(x, y, 2), texel(x, y, 3));
        gpuChannelFormatDesc cd = gpuCreateChannelDesc<uchar4>();
        CK(gpuMallocArray(&arr, &cd, w, w));
        CK(gpuMemcpy2DToArray(arr, 0, 0, px.data(), w * 4, w * 4, w, gpuMemcpyHostToDevice));
        gpuResourceDesc rd; std::memset(&rd, 0, sizeof rd); rd.resType = gpuResourceTypeArray; rd.res.array.array = arr;
        gpuTextureDesc td; std::memset(&td, 0, sizeof td);
        td.addressMode[0] = td.addressMode[1] = gpuAddressModeClamp; td.filterMode = gpuFilterModeLinear;
        td.readMode = gpuReadModeNormalizedFloat; td.normalizedCoords = 1;
        CK(gpuCreateTextureObject(&tex, &rd, &td, nullptr));
        out = dalloc<float>(threads); h.resize(threads);
        want = 0;
        for (unsigned g = 0; g < threads; g++) for (unsigned k = 0; k < per; k++) {
            int tx = (g + k * 977) % 4096, ty = (g / 16 + k * 131) % 4096;
            want += (texel(tx, ty, 0) + texel(tx, ty, 1) + texel(tx, ty, 2) + texel(tx, ty, 3)) / 255.0;
        }
        return true;
    }
    void run() override { tex_k<<<threads / 256, 256>>>(tex, out, per); CK(gpuDeviceSynchronize()); }
    bool check() override { CK(gpuMemcpy(h.data(), out, threads * 4, gpuMemcpyDeviceToHost)); double s = 0; for (float v : h) s += v; return std::fabs(s - want) <= 1e-4 * want; }
    void finish() override { gpuDestroyTextureObject(tex); gpuFreeArray(arr); gpuFree(out); arr = 0; }
};

static std::unique_ptr<Workload> make(const std::string& id) {
    for (int k = 0; k < 5; k++) { auto w = std::make_unique<BabelStream>(k); if (w->id == id) return w; }
    for (int t = 0; t < 4; t++) { auto w = std::make_unique<Gemm>(t); if (w->id == id) return w; }
    std::unique_ptr<Workload> list[] = {
        std::make_unique<Saxpy>(), std::make_unique<Reduce>(), std::make_unique<Scan>(), std::make_unique<Mandel>(),
        std::make_unique<Mix>(), std::make_unique<Launch>(), std::make_unique<Transfer>(), std::make_unique<AllocFree>(),
        std::make_unique<TwoStream>(), std::make_unique<Timer>(), std::make_unique<Texture>()};
    for (auto& w : list) if (w->id == id) return std::move(w);
    return nullptr;
}

static const char* ALL[] = {"babelstream-copy", "babelstream-mul", "babelstream-add", "babelstream-triad", "babelstream-dot",
    "device-saxpy", "wave-reduce", "scan", "gemm-f64", "gemm-f32", "gemm-f16", "gemm-bf16", "mandelbrot-device",
    "mixbench-sweep", "launch-overhead", "kernelbuffer-alloc-free", "host-device-transfer", "two-stream-overlap",
    "kerneltimer-resolution", "texture-sample-rate"};

static const char* area_of(const std::string& id) { return id == "texture-sample-rate" ? "gfx" : "xpu"; }

static int envi(const char* k, int d) { const char* v = std::getenv(k); return v && *v ? std::atoi(v) : d; }

static void row(Workload& w, const std::string& v, int warm, int trials, std::vector<int64_t> s, bool ok, const std::string& ver) {
    std::sort(s.begin(), s.end());
    int64_t sum = 0; for (auto x : s) sum += x;
    size_t nn = s.size(); int64_t med = s[nn / 2];
    double tp = med > 0 && w.input >= 0 ? (double) w.input * 1e9 / (double) med : 0;
    const char* rid = std::getenv("PROFILE_RUN_ID"); const char* ts = std::getenv("PROFILE_RUN_TS");
    std::string note = w.note();
    for (auto& c : note) if (c == ',') c = ';';
    std::printf("1,%s,%s,%s,%s,,-1,,%lld,%s,%s,%s,%s,-O3 -arch=sm_89,%d,%d,%lld,%lld,%lld,%lld,%.3f,%s,-1,-1,-1,-1,-1,%s,%s,%s,%s,%s\n",
        rid ? rid : "local", ts ? ts : "", w.id.c_str(), area_of(w.id), (long long) w.input, w.lang.c_str(), ver.c_str(),
        w.library.c_str(), ver.c_str(), warm, trials, (long long) s[0], (long long) med, (long long) (sum / (int64_t) nn),
        (long long) s[std::min(nn - 1, nn * 95 / 100)], tp, w.unit.c_str(), ok ? "ok" : "invalid", ok ? "true" : "false",
        note.c_str(), v.c_str(), GPU_BACKEND);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "--rows";
    if (mode == "--list") {
        for (auto id : ALL) { auto w = make(id); for (auto& v : w->variants()) std::printf("%s/%s %s\n", area_of(id), id, v.c_str()); }
        return 0;
    }
    if (mode == "--serve") {
        std::unique_ptr<Workload> w; std::string line;
        while (std::getline(std::cin, line)) {
            if (line.rfind("prepare ", 0) == 0) {
                std::string rest = line.substr(8); size_t sp = rest.find(' ');
                std::string id = rest.substr(0, sp), v = sp == std::string::npos ? "" : rest.substr(sp + 1);
                if (w) { w->finish(); w.reset(); }
                w = make(id);
                if (!w) { std::printf("unsupported no workload %s\n", id.c_str()); }
                else if (!w->prepare(v)) { std::printf("unsupported variant %s\n", v.c_str()); }
                else { std::printf("ok %s|devserver %s %s|%s\n", w->lang.c_str(), w->library.c_str(), GPU_BACKEND, toolkit_version().c_str()); }
            } else if (line == "run") { if (w) w->run(); std::printf("ok\n"); }
            else if (line == "check") { std::printf("ok %d\n", w && w->check() ? 1 : 0); }
            else if (line == "quit") { break; }
            else { std::printf("unsupported request\n"); }
            std::fflush(stdout);
        }
        if (w) w->finish();
        return 0;
    }
    std::vector<std::string> ids;
    for (int i = 2; i < argc; i++) ids.push_back(argv[i]);
    if (ids.empty()) for (auto id : ALL) ids.push_back(id);
    int warm = envi("PROFILE_WARMUP", 3), trials = envi("PROFILE_TRIALS", 20);
    std::string ver = toolkit_version();
    for (auto& id : ids) {
        auto w = make(id);
        if (!w) { std::fprintf(stderr, "devserver: unknown id %s\n", id.c_str()); continue; }
        for (auto& v : w->variants()) {
            if (!w->prepare(v)) continue;
            for (int i = 0; i < warm; i++) w->run();
            std::vector<int64_t> s;
            for (int i = 0; i < trials; i++) { int64_t t0 = now_ns(); w->run(); s.push_back(now_ns() - t0); }
            bool ok = w->check();
            row(*w, v, warm, trials, s, ok, ver);
        }
        w->finish();
    }
    return 0;
}
