// GPU matmul competitor: dense N*N float64 C = A*B via rocBLAS (rocblas_dgemm)
// on the GPU (gfx1151 / HIP). Mirrors the Cajeta gpu bench: A = identity,
// B[idx] = idx % 100, so sum(C) == sum(B) is an exact cross-check (identity*B
// == B for any layout; the checksum is layout-independent), while dgemm still
// does the full N^3 work. Emits one CSV row per swept size (columns.txt order +
// trailing backend). Throughput in GFLOP/s. A failed alloc/launch -> skipped row.
#include <hip/hip_runtime.h>
#include <rocblas/rocblas.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const int SIZES[] = {256, 512, 1024, 2048};

static std::string env(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string(d);
}

static void skip(const std::string& rid, const std::string& ts, int n,
                 const char* reason) {
    long sz = (long)n * n;
    std::printf("1,%s,%s,matmul,gpu,,-1,,%ld,cpp,,rocBLAS,,,0,0,-1,-1,-1,-1,,,"
                "-1,-1,-1,-1,-1,skipped,,%s,n%d,hip\n",
                rid.c_str(), ts.c_str(), sz, reason, n);
}

int main() {
    std::string rid = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "0");
    std::string hipver = env("PROFILE_HIP_VERSION", "rocm");

    rocblas_handle handle;
    if (rocblas_create_handle(&handle) != rocblas_status_success) {
        for (int n : SIZES) skip(rid, ts, n, "rocblas_create_handle failed");
        return 0;
    }

    for (int n : SIZES) {
        long elems = (long)n * n;
        std::vector<double> hA(elems), hB(elems), hC(elems, 0.0);
        double bsum = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                long idx = (long)i * n + j;
                hA[idx] = (i == j) ? 1.0 : 0.0;
                hB[idx] = (double)(idx % 100);
                bsum += hB[idx];
            }
        double *dA = nullptr, *dB = nullptr, *dC = nullptr;
        size_t bytes = elems * sizeof(double);
        if (hipMalloc(&dA, bytes) != hipSuccess ||
            hipMalloc(&dB, bytes) != hipSuccess ||
            hipMalloc(&dC, bytes) != hipSuccess) {
            skip(rid, ts, n, "hipMalloc failed");
            hipFree(dA); hipFree(dB); hipFree(dC);
            continue;
        }
        hipMemcpy(dA, hA.data(), bytes, hipMemcpyHostToDevice);
        hipMemcpy(dB, hB.data(), bytes, hipMemcpyHostToDevice);
        const double alpha = 1.0, beta = 0.0;
        auto gemm = [&]() {
            rocblas_dgemm(handle, rocblas_operation_none, rocblas_operation_none,
                          n, n, n, &alpha, dA, n, dB, n, &beta, dC, n);
            hipDeviceSynchronize();
        };
        for (int w = 0; w < 10; ++w) gemm();          // warmup
        std::vector<long> samp;
        samp.reserve(30);
        for (int t = 0; t < 30; ++t) {
            auto t0 = std::chrono::steady_clock::now();
            gemm();
            auto t1 = std::chrono::steady_clock::now();
            samp.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
        hipMemcpy(hC.data(), dC, bytes, hipMemcpyDeviceToHost);
        double csum = 0.0;
        for (long i = 0; i < elems; ++i) csum += hC[i];
        bool ok = (csum == bsum);
        std::sort(samp.begin(), samp.end());
        long mn = samp.front(), med = samp[samp.size() / 2];
        long mean = 0; for (long v : samp) mean += v; mean /= (long)samp.size();
        long p95 = samp[std::min(samp.size() - 1, samp.size() * 95 / 100)];
        double gflops = med > 0 ? (2.0 * n * (double)n * n) / med : 0.0;
        std::printf("1,%s,%s,matmul,gpu,,-1,,%ld,cpp,%s,rocBLAS,%s,-O3,10,30,"
                    "%ld,%ld,%ld,%ld,%.3f,GFLOP/s,-1,-1,-1,-1,-1,%s,%s,,n%d,hip\n",
                    rid.c_str(), ts.c_str(), elems, hipver.c_str(), hipver.c_str(),
                    mn, med, mean, p95, gflops, ok ? "ok" : "invalid",
                    ok ? "true" : "false", n);
        std::fflush(stdout);
        hipFree(dA); hipFree(dB); hipFree(dC);
    }
    rocblas_destroy_handle(handle);
    return 0;
}
