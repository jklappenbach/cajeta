// cajeta-ml-gpu Unit 5 (5.1.2; spec §3.7): per-element host reads through Tensor cost
// about what a raw array loop costs in a --release exe. The ratio is measured here;
// the 1.5x bound is asserted on a quiet box (CAJETA_QUIET_BOX=1), and a 4x bound
// always, which is where the pre-inliner 12x would fail.
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

const char* kBench =
    "package app;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.time.Clock;\n"
    "public class Main {\n"
    "    static float64 sumGet(Tensor<float64> t, int64 n) {\n"
    "        float64 s = 0.0;\n"
    "        int64 i = 0;\n"
    "        while (i < n) { s = s + t.get1(i); i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    static float64 sumRaw(float64[] d, int64 n) {\n"
    "        float64 s = 0.0;\n"
    "        int64 i = 0;\n"
    "        while (i < n) { s = s + d[(int32) i]; i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int64 n = 10000000L;\n"
    "        float64[] d = heap float64[n];\n"
    "        int64 i = 0;\n"
    "        while (i < n) { d[(int32) i] = (float64) (i % 7); i = i + 1; }\n"
    "        int64[] shape = heap int64[1];\n"
    "        shape[0] = n;\n"
    "        Tensor<float64> t #= Tensor.of<float64>(d, shape);\n"
    "        int64 bestGet = 0L;\n"
    "        int64 bestRaw = 0L;\n"
    "        float64 check = 0.0;\n"
    "        int32 rep = 0;\n"
    "        while (rep < 5) {\n"
    "            int64 a = Clock.nanoTime();\n"
    "            check = check + Main.sumRaw(d, n);\n"
    "            int64 b = Clock.nanoTime();\n"
    "            check = check - Main.sumGet(t, n);\n"
    "            int64 c = Clock.nanoTime();\n"
    "            if (bestRaw == 0L || b - a < bestRaw) { bestRaw = b - a; }\n"
    "            if (bestGet == 0L || c - b < bestGet) { bestGet = c - b; }\n"
    "            rep = rep + 1;\n"
    "        }\n"
    "        int64 ratio100 = (bestGet * 100L) / bestRaw;\n"
    "        System.stdout.println(\"ratio100 \" + ratio100 + \" raw_ns \" + bestRaw + \" get_ns \" + bestGet + \" check \" + (int64) check);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

long fieldAfter(const std::string& out, const std::string& key) {
    auto p = out.find(key);
    if (p == std::string::npos) return -1;
    return std::atol(out.c_str() + p + key.size());
}

}  // namespace

// 5.1.2: get1 over 1e7 elements against the raw loop, best of five, in --release.
TEST(TensorHostAccessBench, perElementReadIsWithinBoundOfARawLoop) {
    Workspace w;
    w.write("src/app/Main.cajeta", kBench);
    Built b = compile(w, "rel", "--emit=exe --release", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    Ran r = run(b);
    ASSERT_EQ(r.rc, 0) << r.err;
    long ratio100 = fieldAfter(r.out, "ratio100 ");
    ASSERT_GT(ratio100, 0) << r.out;
    EXPECT_EQ(fieldAfter(r.out, "check "), 0) << r.out;
    std::cout << "[host-access] " << r.out;
    const double bound = std::getenv("CAJETA_QUIET_BOX") ? 1.5 : 4.0;
    EXPECT_LE(ratio100 / 100.0, bound) << r.out;
}
