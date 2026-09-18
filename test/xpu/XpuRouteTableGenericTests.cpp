//
// XpuRouteTableGenericTests — the row type is not weight-format-shaped.
//
// Spec §6 claims the mechanism is (data format × regime × capability) → kernel
// and that only the format axis is LLM-specific. That is a claim about the row
// type, and it is cheapest to falsify BEFORE a registrant depends on it — so
// these run in the same unit that freezes the type, not in a later one.
//
// Two registrants that share nothing with a quantized weight: a dense GEMM
// picking by dtype with forward and backward regimes (the cajeta-ml shape),
// and a histogram picking by bin count (the cajeta-xgboost shape, whose axis
// is an ordinary integer and not an enumeration of formats at all).
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {

int run(const std::string& types, const std::string& body) {
    std::string program =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.xpu.Capability;\n"
        "import cajeta.xpu.Regime;\n"
        "import cajeta.xpu.Route;\n"
        "import cajeta.xpu.RouteCall;\n"
        "import cajeta.xpu.RouteQuery;\n"
        "import cajeta.xpu.RouteTable;\n"
        + types +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        RouteTable t = heap RouteTable();\n"
        + body +
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.T", o);
    if (!jit) return -100;
    auto fn = jit->lookup<int (*)()>("run");
    if (!fn) return -101;
    return fn();
}

// Dtype axis, forward/backward regimes: the training shape. `Dtype` is the
// registrant's enum and the layer never sees it — the row converts at the
// boundary, which is the honest version of "an opaque ty".
const char* kMlTypes =
    "public enum Dtype { F32, F16, BF16 }\n"
    "public final class GemmQuery extends RouteQuery {\n"
    "    public int32 m;\n"
    "    public int32 n;\n"
    "    public GemmQuery(Regime g, Dtype d, int32 m, int32 n) {\n"
    "        this.regime = g;\n"
    "        this.ty = GemmQuery.tag(d);\n"
    "        this.m = m;\n"
    "        this.n = n;\n"
    "    }\n"
    "    public static int32 tag(Dtype d) {\n"
    "        if (d == Dtype.F32) { return 0; }\n"
    "        if (d == Dtype.F16) { return 1; }\n"
    "        return 2;\n"
    "    }\n"
    "}\n"
    "public final class GemmRow implements Route {\n"
    "    String nm;\n"
    "    Regime rg;\n"
    "    Dtype dt;\n"
    "    int32 tile;\n"
    "    public GemmRow(String n, Regime g, Dtype d, int32 tile) {\n"
    "        this.nm #= n;\n"
    "        this.rg = g;\n"
    "        this.dt = d;\n"
    "        this.tile = tile;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return this.rg; }\n"
    "    public int32 format() { return GemmQuery.tag(this.dt); }\n"
    "    public int32 priority() { return 10; }\n"
    "    public String shapeRefusal(RouteQuery q) {\n"
    "        if (q instanceof GemmQuery g) {\n"
    "            if (g.m % this.tile != 0) { return \"m not a tile\"; }\n"
    "            if (g.n % this.tile != 0) { return \"n not a tile\"; }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a gemm query\";\n"
    "    }\n"
    "    public Capability[] needs() { return null; }\n"
    "    public String readyRefusal(RouteCall c) { return null; }\n"
    "    public void dispatch(RouteCall c) { return; }\n"
    "}\n";

// Bin-count axis: the histogram shape, where the axis is an ordinary integer
// with no enumeration behind it at all.
const char* kBinTypes =
    "public final class HistQuery extends RouteQuery {\n"
    "    public int32 features;\n"
    "    public HistQuery(Regime g, int32 bins, int32 features) {\n"
    "        this.regime = g;\n"
    "        this.ty = bins;\n"
    "        this.features = features;\n"
    "    }\n"
    "}\n"
    "public final class HistRow implements Route {\n"
    "    String nm;\n"
    "    int32 bins;\n"
    "    int32 minFeatures;\n"
    "    public HistRow(String n, int32 bins, int32 minF) {\n"
    "        this.nm #= n;\n"
    "        this.bins = bins;\n"
    "        this.minFeatures = minF;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return Regime.PrefillBatch; }\n"
    "    public int32 format() { return this.bins; }\n"
    "    public int32 priority() { return 10; }\n"
    "    public String shapeRefusal(RouteQuery q) {\n"
    "        if (q instanceof HistQuery h) {\n"
    "            if (h.features < this.minFeatures) {\n"
    "                return \"too few features\";\n"
    "            }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a histogram query\";\n"
    "    }\n"
    "    public Capability[] needs() { return null; }\n"
    "    public String readyRefusal(RouteCall c) { return null; }\n"
    "    public void dispatch(RouteCall c) { return; }\n"
    "}\n";

} // namespace

// A dtype axis with forward and backward regimes resolves, and the backward
// row for one dtype is a different row from its forward one.
TEST(XpuRouteTableGeneric, aDtypeAxisWithTrainingRegimes) {
    int rc = run(kMlTypes,
        "        t.register(heap GemmRow(\"bf16-fwd\", Regime.PrefillBatch,\n"
        "            Dtype.BF16, 16));\n"
        "        t.register(heap GemmRow(\"bf16-bwd\", Regime.FusedTail,\n"
        "            Dtype.BF16, 16));\n"
        "        t.register(heap GemmRow(\"f32-fwd\", Regime.PrefillBatch,\n"
        "            Dtype.F32, 8));\n"
        "        GemmQuery fwd = heap GemmQuery(Regime.PrefillBatch,\n"
        "            Dtype.BF16, 128, 256);\n"
        "        Route a = t.admissible(fwd);\n"
        "        if (a == null || !a.name().equals(\"bf16-fwd\")) { return 1; }\n"
        "        GemmQuery bwd = heap GemmQuery(Regime.FusedTail,\n"
        "            Dtype.BF16, 128, 256);\n"
        "        Route b = t.admissible(bwd);\n"
        "        if (b == null || !b.name().equals(\"bf16-bwd\")) { return 2; }\n"
        "        GemmQuery ragged = heap GemmQuery(Regime.PrefillBatch,\n"
        "            Dtype.BF16, 100, 256);\n"
        "        if (t.admissible(ragged) != null) { return 3; }\n"
        "        GemmQuery f16 = heap GemmQuery(Regime.PrefillBatch,\n"
        "            Dtype.F16, 128, 256);\n"
        "        if (t.admissible(f16) != null) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// A bin count is not an enumeration of formats, and the row type must not
// require it to be. If this needed a cast that lies, the row type is wrong.
TEST(XpuRouteTableGeneric, aBinCountAxisIsNotAnEnumerationOfFormats) {
    int rc = run(kBinTypes,
        "        t.register(heap HistRow(\"hist256-wide\", 256, 64));\n"
        "        t.register(heap HistRow(\"hist64\", 64, 0));\n"
        "        HistQuery wide = heap HistQuery(Regime.PrefillBatch, 256, 128);\n"
        "        Route a = t.admissible(wide);\n"
        "        if (a == null || !a.name().equals(\"hist256-wide\")) { return 1; }\n"
        "        HistQuery narrow = heap HistQuery(Regime.PrefillBatch, 256, 8);\n"
        "        if (t.admissible(narrow) != null) { return 2; }\n"
        "        HistQuery small = heap HistQuery(Regime.PrefillBatch, 64, 8);\n"
        "        Route b = t.admissible(small);\n"
        "        if (b == null || !b.name().equals(\"hist64\")) { return 3; }\n"
        "        HistQuery absent = heap HistQuery(Regime.PrefillBatch, 128, 8);\n"
        "        if (t.admissible(absent) != null) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}
