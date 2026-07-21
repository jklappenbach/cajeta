//
// nucleo-nn-optim Unit 2 — Parameter<T>, Module, reflection collection
// (plan 2.1.x; spec §2, §3; resolutions N1–N4).
//
// A module OWNS its Parameter fields and sub-modules; parameters() walks the
// typed fields via reflection (Class.of + field enumeration + getRef +
// instanceof) — no per-module registration. Order is declared order,
// parent-first, recursing depth-first at each sub-module field (§3.1 stable
// order). parameterNames() gives the dotted paths in the SAME order (§3.4 —
// the v1 namedParameters surface: a parallel OWNING ArrayList<String>). Plain Tensor fields are buffers (§3.6).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// The shared module tree: Net { w0: Param, head: Inner { weight, bias },
// runningStat: buffer }. Params carry distinguishable values so ORDER is
// observable: w0=1, head.weight=2, head.bias=3, runningStat=9.
std::string treeSrc(const std::string& runBody) {
    return std::string(
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "import cajeta.nucleo.nn.Module;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.nucleo.nn.Parameter;\n"
        "public class Inner extends Module {\n"
        "    Parameter weight;\n"
        "    Parameter bias;\n"
        "    public Inner() {\n"
        "        int64[] s = {2};\n"
        "        this.weight = heap Parameter(Tensor.full<float32>(s, 2.0f));\n"
        "        this.bias = heap Parameter(Tensor.full<float32>(s, 3.0f));\n"
        "    }\n"
        "}\n"
        "public class Net extends Module {\n"
        "    Parameter w0;\n"
        "    Inner head;\n"
        "    Tensor<float32> runningStat;\n"
        "    public Net() {\n"
        "        int64[] s = {2};\n"
        "        this.w0 = heap Parameter(Tensor.full<float32>(s, 1.0f));\n"
        "        this.head = heap Inner();\n"
        "        this.runningStat = Tensor.full<float32>(s, 9.0f);\n"
        "    }\n"
        "}\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        ") + runBody + "\n"
        "    }\n"
        "}\n";
}

float runTree(const std::string& body) {
    auto jit = CajetaJit::compile(treeSrc(body), "test.T");
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1e9f;
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1e9f;
    return fn();
}

} // namespace

// 2.1.1 — parameters() returns every owned + sub-module parameter, declared
// order, parent-first: [w0, head.weight, head.bias] = values [1, 2, 3].
TEST(ModuleTests, parametersWalkAllStableOrder) {
    float r = runTree(
        "Net net = heap Net();\n"
        "        Parameter[] ps = net.parameters();\n"
        "        return (float32)(ps.count()) * 1000.0f\n"
        "            + ps[0].get().get1(0) * 100.0f\n"
        "            + ps[1].get().get1(0) * 10.0f\n"
        "            + ps[2].get().get1(0);");
    // 3 params -> 3000; order 1,2,3 -> +100 +20 +3
    EXPECT_NEAR(r, 3123.0f, 1e-3f);
}

// 2.1.2 — parameterNames(): dotted paths in the same order.
TEST(ModuleTests, parameterNamesDottedPaths) {
    float r = runTree(
        "Net net = heap Net();\n"
        "        ArrayList<String> names = net.parameterNames();\n"
        "        float32 acc = (float32)(names.count()) * 1000.0f;\n"
        "        if (names.get(0).equals(\"w0\")) { acc = acc + 100.0f; }\n"
        "        if (names.get(1).equals(\"head.weight\")) { acc = acc + 10.0f; }\n"
        "        if (names.get(2).equals(\"head.bias\")) { acc = acc + 1.0f; }\n"
        "        return acc;");
    EXPECT_NEAR(r, 3111.0f, 1e-3f);
}

// 2.1.3 — a plain Tensor field is a buffer: excluded from parameters(),
// present in buffers().
TEST(ModuleTests, buffersSeparateFromParameters) {
    float r = runTree(
        "Net net = heap Net();\n"
        "        Tensor<float32>[] bs = net.buffers();\n"
        "        return (float32)(bs.count()) * 100.0f + bs[0].get1(0);");
    // 1 buffer -> 100; value 9 -> 109
    EXPECT_NEAR(r, 109.0f, 1e-3f);
}

// 2.1.4 — subset collection: head.parameters() is just the head's two; the
// freeze story is collection choice, not a per-tensor flag.
TEST(ModuleTests, subModuleSubsetCollection) {
    float r = runTree(
        "Net net = heap Net();\n"
        "        Parameter[] ps = net.head.parameters();\n"
        "        return (float32)(ps.count()) * 100.0f\n"
        "            + ps[0].get().get1(0) * 10.0f + ps[1].get().get1(0);");
    // 2 params -> 200; values 2,3 -> +20 +3
    EXPECT_NEAR(r, 223.0f, 1e-3f);
}

// 2.1.5 — lifetime (N3): the module owns its parameters; a collection
// outstanding while the module lives reads them; after the owning scope
// exits, the live count returns to its baseline (no leak, no double-free —
// a double-free would trip the runtime).
TEST(ModuleTests, ownershipNoLeakNoDoubleFree) {
    float r = runTree(
        "int64 base = Cajeta.liveCount();\n"
        "        float32 sum = 0.0f;\n"
        "        scope {\n"
        "            Net net = heap Net();\n"
        "            Parameter[] ps = net.parameters();\n"
        "            sum = ps[0].get().get1(0) + ps[1].get().get1(0)\n"
        "                + ps[2].get().get1(0);\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return sum + (float32)(leaked) * 1000.0f;");
    // sum 1+2+3 = 6; leaked 0
    EXPECT_NEAR(r, 6.0f, 1e-3f);
}
