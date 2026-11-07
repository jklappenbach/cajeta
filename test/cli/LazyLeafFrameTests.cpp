// cajeta-ml-gpu Unit 5 (5.2.2; spec §3.7): an accessor-shaped method (no calls outside
// its throw statements, no loops) pushes its line frame only on a throw path, so an
// inlined accessor in a hot loop costs no shadow-stack traffic; its exception traces
// are unchanged, and a leaf with a loop keeps its frame for the profiler.
#include <gtest/gtest.h>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

const char* kProgram =
    "package app;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.PlacementException;\n"
    "import cajeta.error.StackFrame;\n"
    "public class Main {\n"
    "    static float64 sumGet(Tensor<float64> t, int64 n) {\n"
    "        float64 s = 0.0;\n"
    "        int64 i = 0;\n"
    "        while (i < n) { s = s + t.get1(i); i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    static int64 spin(int64 n) { int64 s = 0; int64 i = 0; while (i < n) { s = (s + i) & 1023; i = i + 1; } return s; }\n"
    "    public static int32 main(String[] args) {\n"
    "        int64[] shape = heap int64[1];\n"
    "        shape[0] = 8;\n"
    "        Tensor<float64> t #= Tensor.zeros<float64>(shape);\n"
    "        int64 i = 0;\n"
    "        while (i < 8) { t.flatSet(i, (float64) i); i = i + 1; }\n"
    "        System.stdout.println(\"sum \" + (int64) Main.sumGet(t, 8) + \" spin \" + Main.spin(100));\n"
    "        t.gpu();\n"
    "        try { float64 v = t.get1(2); System.stdout.println(\"no throw \" + v); }\n"
    "        catch (PlacementException ex) {\n"
    "            StackFrame[] fs #= ex.getStackTrace();\n"
    "            int32 k = 0;\n"
    "            while (k < (int32) fs.count()) { System.stdout.println(\"frame \" + fs[k].declaringType + \".\" + fs[k].method); k = k + 1; }\n"
    "        }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

/** The IR of modules other than the stdlib's. */
std::string userIr(const Built& b) {
    std::string all;
    for (auto& f : fs::directory_iterator(b.dump))
        if (f.path().extension() == ".ll" && f.path().filename().string().find("__stdlib__") == std::string::npos)
            all += slurp(f.path());
    return all;
}


/** The first (entry) block of a function body and the block holding its `ret`. */
std::string entryBlock(const std::string& body) {
    size_t s = body.find("\n");
    if (s == std::string::npos) return "";
    size_t e = body.find("\n\n", s);
    return body.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
std::string retBlock(const std::string& body) {
    size_t r = body.find("\n  ret ");
    if (r == std::string::npos) return "";
    size_t s = body.rfind("\n\n", r);
    return body.substr(s == std::string::npos ? 0 : s, r - (s == std::string::npos ? 0 : s) + 8);
}

size_t countOf(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + needle.size())) ++n;
    return n;
}

}  // namespace

// 5.2.2: in --release the accessor get1 pushes no frame at entry and pops none at its
// return (its only frame traffic sits on the throw path), so the loop that inlines it
// carries none; the loop leaf `spin` keeps its frame at entry.
TEST(LazyLeafFrame, anAccessorPushesItsFrameOnlyOnTheThrowPath) {
    Workspace w;
    w.write("src/app/Main.cajeta", kProgram);
    Built b = compile(w, "rel", "--emit=exe --release --xpu-backend=cpu", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    std::string all = dumpedIr(b);
    std::string get1 = bodiesOf(all, "cajeta.math.Tensor<float64>::get1");
    ASSERT_FALSE(get1.empty());
    EXPECT_EQ(entryBlock(get1).find("shadow.top"), std::string::npos) << get1;
    EXPECT_EQ(retBlock(get1).find("shadow.top"), std::string::npos) << get1;
    EXPECT_GE(countOf(get1, "shadow.top"), 1u) << get1;
    std::string spin = bodiesOf(userIr(b), "app.Main::spin");
    ASSERT_FALSE(spin.empty());
    EXPECT_NE(entryBlock(spin).find("shadow"), std::string::npos) << spin;
}

// 5.2.2: the trace of an exception thrown from an accessor still names the accessor
// frame (Tensor.get1), at O0 where traces are captured.
TEST(LazyLeafFrame, aThrowFromAnAccessorKeepsItsFramesInTheTrace) {
    Workspace w;
    w.write("src/app/Main.cajeta", kProgram);
    Built b = compile(w, "o0", "--emit=exe --xpu-backend=cpu", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    Ran r = run(b, cajeta_env_prefix({{"CAJETA_XPU_BACKEND", "cpu"}}));
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("sum 28 spin"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("frame cajeta.math.Tensor<float64>.get1"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("frame app.Main.main"), std::string::npos) << r.out;
}
