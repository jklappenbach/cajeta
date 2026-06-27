//
// benchmark-gap-sweep Unit 1 — vectorized CLBG mandelbrot.
// The scalar per-pixel escape-time loop is the baseline the C++/Rust CLBG entries
// auto-vectorize at -O3 -march=native. This processes 8 pixels of a row at once in
// Vector<float64,8>: cr = (iota + px) * (2/n) - 1.5, scalar ci, iterate 50x with a
// MONOTONIC membership mask (once |z|^2 > 4 a lane escapes and stays escaped, so
// member *= (mag<=4) via select is exact), early-out when all 8 lanes have escaped,
// and count += vsum8f64(member). The vectorized in-set count must equal the scalar
// reference count bit-for-bit (the float64 arithmetic is identical per lane; only the
// horizontal vsum of an exact-integer 0/1 vector is added) — that equality is the
// correctness oracle the bench's run() port is guarded against.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Builds a module with vectorized runVec() and scalar runScalar() over a w-wide grid,
// each returning the in-set pixel count. Returns {vec, scalar}.
std::pair<long long, long long> mandelbrotCounts(int w) {
    std::string ws = std::to_string(w);
    std::string src =
        "package test;\n"
        "public final class MB {\n"
        // --- vectorized: 8 px/block ---
        "    public static int64 runVec() {\n"
        "        int32 n = " + ws + ";\n"
        "        int64 count = 0;\n"
        "        int32 lim = n - (n % 8);\n"
        "        Vector<float64,8> iota = stack Vector<float64,8>(0.0,1.0,2.0,3.0,4.0,5.0,6.0,7.0);\n"
        "        Vector<float64,8> ones = stack Vector<float64,8>(1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0);\n"
        "        Vector<float64,8> zeros = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        float64 inv = 2.0 / (float64) n;\n"
        "        int32 py = 0;\n"
        "        while (py < n) {\n"
        "            float64 ci = (2.0 * (float64) py / (float64) n) - 1.0;\n"
        "            int32 px = 0;\n"
        "            while (px < lim) {\n"
        "                Vector<float64,8> cr = (iota + (float64) px) * inv + (-1.5);\n"
        "                Vector<float64,8> zr = zeros;\n"
        "                Vector<float64,8> zi = zeros;\n"
        "                Vector<float64,8> member = ones;\n"
        "                int32 iter = 0;\n"
        "                while (iter < 50) {\n"
        "                    Vector<float64,8> nzr = zr * zr - zi * zi + cr;\n"
        "                    Vector<float64,8> nzi = (zr * zi) * 2.0 + ci;\n"
        "                    zr = nzr;\n"
        "                    zi = nzi;\n"
        "                    Vector<float64,8> mag = zr * zr + zi * zi;\n"
        "                    member = (mag <= 4.0).select(member, zeros);\n"
        "                    iter = iter + 1;\n"
        "                    if ((member == 0.0).all()) { iter = 50; }\n"
        "                }\n"
        "                count = count + (int64) Cajeta.vsum8f64(member);\n"
        "                px = px + 8;\n"
        "            }\n"
        "            while (px < n) {\n"
        "                float64 cr2 = (2.0 * (float64) px / (float64) n) - 1.5;\n"
        "                float64 zr2 = 0.0; float64 zi2 = 0.0;\n"
        "                int32 it2 = 0; boolean mem = true;\n"
        "                while (it2 < 50) {\n"
        "                    float64 nzr2 = zr2 * zr2 - zi2 * zi2 + cr2;\n"
        "                    float64 nzi2 = 2.0 * zr2 * zi2 + ci;\n"
        "                    zr2 = nzr2; zi2 = nzi2;\n"
        "                    if (zr2 * zr2 + zi2 * zi2 > 4.0) { mem = false; it2 = 50; } else { it2 = it2 + 1; }\n"
        "                }\n"
        "                if (mem) { count = count + 1; }\n"
        "                px = px + 1;\n"
        "            }\n"
        "            py = py + 1;\n"
        "        }\n"
        "        return count;\n"
        "    }\n"
        // --- scalar reference ---
        "    public static int64 runScalar() {\n"
        "        int32 n = " + ws + ";\n"
        "        int64 count = 0;\n"
        "        int32 py = 0;\n"
        "        while (py < n) {\n"
        "            int32 px = 0;\n"
        "            while (px < n) {\n"
        "                float64 cr = (2.0 * (float64) px / (float64) n) - 1.5;\n"
        "                float64 ci = (2.0 * (float64) py / (float64) n) - 1.0;\n"
        "                float64 zr = 0.0; float64 zi = 0.0;\n"
        "                int32 iter = 0; boolean member = true;\n"
        "                while (iter < 50) {\n"
        "                    float64 nzr = zr * zr - zi * zi + cr;\n"
        "                    float64 nzi = 2.0 * zr * zi + ci;\n"
        "                    zr = nzr; zi = nzi;\n"
        "                    if (zr * zr + zi * zi > 4.0) { member = false; iter = 50; } else { iter = iter + 1; }\n"
        "                }\n"
        "                if (member) { count = count + 1; }\n"
        "                px = px + 1;\n"
        "            }\n"
        "            py = py + 1;\n"
        "        }\n"
        "        return count;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.MB");
    auto fv = jit->lookup<long long (*)()>("runVec");
    auto fs = jit->lookup<long long (*)()>("runScalar");
    long long v = fv ? fv() : -1;
    long long s = fs ? fs() : -2;
    return {v, s};
}

} // namespace

// 1.1.a — vectorized in-set count equals the scalar reference, and is a strict subset
// of the grid. w=160 is a multiple of 8 (vectorized-only path, no scalar tail).
TEST(MandelbrotKernelTests, vectorEqualsScalarNoTail) {
    auto [v, s] = mandelbrotCounts(160);
    EXPECT_EQ(v, s);
    EXPECT_GT(v, 0);
    EXPECT_LT(v, 160LL * 160LL);
}

// w=100 exercises the scalar tail (100 % 8 == 4) alongside the vector body.
TEST(MandelbrotKernelTests, vectorEqualsScalarWithTail) {
    auto [v, s] = mandelbrotCounts(100);
    EXPECT_EQ(v, s);
    EXPECT_GT(v, 0);
    EXPECT_LT(v, 100LL * 100LL);
}
