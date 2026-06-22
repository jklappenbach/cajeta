//
// kernel-vector-loadstore Unit 7 — host-array vload/vstore parity.
// The width-generic `arr.vload<N>(i)` / `arr.vstore(i, v)` surface on a host
// array shares the same host-header (`{i64 size, [0 x elem]}`, data at +8)
// addressing as the legacy `Cajeta.vload8f64`/`vstore8f64` intrinsics, but is
// type- and width-generic and reads identically to the kernel-buffer form
// (only the receiver type differs). Traces kernel-vector-loadstore-spec.md §7.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<int64_t (*)()>("run");
    return f ? f() : -1;
}

const char* f64Head =
    "package test;\n"
    "public final class D {\n"
    "    public static float64 run() {\n"
    "        float64[] a = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };\n";
const char* tail = "    }\n}\n";

} // namespace

// 7.1.1 — `a.vload<8>(i)` on a host float64[] matches the legacy
// `Cajeta.vload8f64`: load 8 lanes, square, horizontal-sum -> Σ k² = 204.
TEST(HostArrayVloadTests, vload8MatchesLegacyF64) {
    std::string src = std::string(f64Head) +
        "        Vector<float64,8> v = a.vload<8>(0);\n"
        "        return Cajeta.vsum8f64(v * v);\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 204.0);
}

// 7.1.1 — `a.vstore(i, v)` roundtrip on a host float64[]: store squares back,
// reload, sum + spot-check a lane. a -> {1,4,9,...,64}; a[3]=16, Σ=204 -> 220.
TEST(HostArrayVloadTests, vstoreRoundtripF64) {
    std::string src = std::string(f64Head) +
        "        Vector<float64,8> v = a.vload<8>(0);\n"
        "        a.vstore(0, v * v);\n"
        "        return a[3] + Cajeta.vsum8f64(a.vload<8>(0));\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 220.0);
}

// 7.1.1 — width/type generality: float32[] with width 4. Load {1,2,3,4},
// double, store, reload, sum -> 2*(1+2+3+4) = 20.
TEST(HostArrayVloadTests, vload4Float32) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        float32[] a = { 1.0f, 2.0f, 3.0f, 4.0f };\n"
        "        Vector<float32,4> v = a.vload<4>(0);\n"
        "        a.vstore(0, v + v);\n"
        "        Vector<float32,4> r = a.vload<4>(0);\n"
        "        return (float64)(r.x + r.y + r.z + r.w);\n"
        "    }\n}\n";
    EXPECT_DOUBLE_EQ(runF64(src), 20.0);
}

// 7.1.1 — integer type: int64[] with width 4. Load {10,20,30,40}, store the
// pairwise sum-with-self, reload, reduce by summing lanes -> 2*(10+20+30+40)=200.
TEST(HostArrayVloadTests, vload4Int64) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64[] a = { 10, 20, 30, 40 };\n"
        "        Vector<int64,4> v = a.vload<4>(0);\n"
        "        a.vstore(0, v + v);\n"
        "        return a[0] + a[1] + a[2] + a[3];\n"
        "    }\n}\n";
    EXPECT_EQ(runI64(src), 200);
}

// 7.3.1 — the legacy fixed-name intrinsic still compiles (deprecated alias):
// the same Σ k² = 204 through `Cajeta.vload8f64`.
TEST(HostArrayVloadTests, legacyVload8f64StillCompiles) {
    std::string src = std::string(f64Head) +
        "        Vector<float64,8> v = Cajeta.vload8f64(a, 0);\n"
        "        return Cajeta.vsum8f64(v * v);\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 204.0);
}
