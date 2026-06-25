// Unit 3 (benchmark-fidelity-plan) — correctness guard for the @Kernel matmul
// that the profile suite's MatMulBench runs on the CPU kernel-pool path. The
// kernel keeps a Vector<float64,8> C accumulator per (row, 8-col block) and
// FMAs B[k,jb:jb+8] * A[row,k] across k. These two JIT tests pin the kernel's
// result on the CPU backend: (1) A=identity ⇒ C==B exactly, (2) a non-identity
// A matches a scalar triple-loop reference summed in the same k order.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// The kernel under test (verbatim from samples/matmul-kernel + the profile
// port) plus a per-test host driver appended as the body of run().
std::string matmulProgram(const std::string& runBody) {
    return
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public final class MM {\n"
        "    @Kernel\n"
        "    public static void matmul(KernelBuffer<float64> c, KernelBuffer<float64> a,\n"
        "                              KernelBuffer<float64> b, uint32 n) {\n"
        "        uint32 nblk = n / 8;\n"
        "        uint32 t = KernelThread.globalIdX();\n"
        "        if (t < n * nblk) {\n"
        "            uint32 row = t / nblk;\n"
        "            uint32 jb = (t % nblk) * 8;\n"
        "            uint32 rBase = row * n;\n"
        "            Vector<float64,8> cv =\n"
        "                stack Vector<float64,8>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);\n"
        "            uint32 k = 0;\n"
        "            while (k < n) {\n"
        "                Vector<float64,8> bv = b.vload<8>(k * n + jb);\n"
        "                cv = cv + bv * a[rBase + k];\n"
        "                k = k + 1;\n"
        "            }\n"
        "            c.vstore(rBase + jb, cv);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n" + runBody +
        "    }\n"
        "}\n";
}

// Launch helper shared by both drivers: n=8 (one 8-col block per row, 8 work-
// items), grid sized to cover them.
const char* kLaunch =
    "        KernelBuffer<float64> a = heap KernelBuffer<float64>(sz);\n"
    "        KernelBuffer<float64> b = heap KernelBuffer<float64>(sz);\n"
    "        KernelBuffer<float64> c = heap KernelBuffer<float64>(sz);\n"
    "        a.upload(ha);\n"
    "        b.upload(hb);\n"
    "        c.upload(hc);\n"
    "        KernelStream s = KernelStream.current();\n"
    "        uint32 total = n * (n / 8);\n"
    "        matmul.launch(s, grid: [(total + 63) / 64], block: [64])(c, a, b, n);\n"
    "        s.sync();\n"
    "        c.download(hc);\n";

} // namespace

// 3.a.1 — A=identity ⇒ C==B exactly (checksum equality, the bench's cross-check).
TEST(XpuMatmulKernel, identityEqualsB) {
    std::string body =
        "        uint32 n = 8;\n"
        "        int32 sz = (int32) (n * n);\n"
        "        float64[] ha = heap float64[sz];\n"
        "        float64[] hb = heap float64[sz];\n"
        "        float64[] hc = heap float64[sz];\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) n) {\n"
        "            int32 j = 0;\n"
        "            while (j < (int32) n) {\n"
        "                int32 idx = i * (int32) n + j;\n"
        "                if (i == j) { ha[idx] = 1.0; } else { ha[idx] = 0.0; }\n"
        "                hb[idx] = (float64) (idx % 13);\n"
        "                hc[idx] = 0.0;\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        + std::string(kLaunch) +
        "        int32 m = 0;\n"
        "        while (m < sz) {\n"
        "            if (hc[m] != hb[m]) { return 0; }\n"
        "            m = m + 1;\n"
        "        }\n"
        "        return 1;\n";
    auto jit = CajetaJit::compile(matmulProgram(body), "test.MM", cpuOptions());
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 3.a.2 — non-identity A matches a scalar triple-loop reference (same k order).
TEST(XpuMatmulKernel, nonIdentityMatchesScalar) {
    std::string body =
        "        uint32 n = 8;\n"
        "        int32 sz = (int32) (n * n);\n"
        "        float64[] ha = heap float64[sz];\n"
        "        float64[] hb = heap float64[sz];\n"
        "        float64[] hc = heap float64[sz];\n"
        "        float64[] href = heap float64[sz];\n"
        "        int32 i = 0;\n"
        "        while (i < sz) {\n"
        "            ha[i] = (float64) (i % 7);\n"
        "            hb[i] = (float64) ((i * 3) % 11);\n"
        "            hc[i] = 0.0;\n"
        "            i = i + 1;\n"
        "        }\n"
        // scalar reference C[r,col] = sum_k A[r,k]*B[k,col], same accumulation
        // order as the kernel (k ascending) so float64 results are bit-exact.
        "        int32 r = 0;\n"
        "        while (r < (int32) n) {\n"
        "            int32 col = 0;\n"
        "            while (col < (int32) n) {\n"
        "                float64 acc = 0.0;\n"
        "                int32 k = 0;\n"
        "                while (k < (int32) n) {\n"
        "                    acc = acc + hb[k * (int32) n + col] * ha[r * (int32) n + k];\n"
        "                    k = k + 1;\n"
        "                }\n"
        "                href[r * (int32) n + col] = acc;\n"
        "                col = col + 1;\n"
        "            }\n"
        "            r = r + 1;\n"
        "        }\n"
        + std::string(kLaunch) +
        "        int32 m = 0;\n"
        "        while (m < sz) {\n"
        "            if (hc[m] != href[m]) { return 0; }\n"
        "            m = m + 1;\n"
        "        }\n"
        "        return 1;\n";
    auto jit = CajetaJit::compile(matmulProgram(body), "test.MM", cpuOptions());
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
