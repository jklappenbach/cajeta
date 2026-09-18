//
// XpuRouteTableTests — the selection contract's rows and resolution.
//
// A route table exists because the knowledge of which kernel serves which
// format kept being restated from memory, once per route, in whichever file
// the route lived (route-table spec §1.1). Two properties carry that, and
// both are asserted here: a row is ONE kernel variant serving ONE format, so
// there is no arm to omit; and admission has a static half the audit can walk
// with nothing bound and a runtime half it cannot, kept apart by the type of
// the question asked (spec §3.0).
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {

// A weight-shaped registrant: its static facts are a width, its runtime state
// is whether a slab bound. `fits` is handed a query and so has no way to read
// `slabBound` even by accident — there is no receiver in a query.
const char* kWeightRegistrant =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Capability;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.Regime;\n"
    "import cajeta.xpu.Route;\n"
    "import cajeta.xpu.RouteCall;\n"
    "import cajeta.xpu.RouteQuery;\n"
    "import cajeta.xpu.RouteTable;\n"
    "\n"
    "public final class WQuery extends RouteQuery {\n"
    "    public int32 inDim;\n"
    "    public WQuery(Regime g, int32 ty, int32 inDim) {\n"
    "        this.regime = g;\n"
    "        this.ty = ty;\n"
    "        this.inDim = inDim;\n"
    "    }\n"
    "}\n"
    "\n"
    "public final class WCall extends RouteCall {\n"
    "    public boolean slabBound;\n"
    "    public WCall(WQuery q, boolean bound) {\n"
    "        this.query #= q;\n"
    "        this.slabBound = bound;\n"
    "    }\n"
    "}\n"
    "\n"
    "public final class V implements Route {\n"
    "    public static int32 dispatched;\n"
    "    String nm;\n"
    "    Regime rg;\n"
    "    int32 fmt;\n"
    "    int32 pri;\n"
    "    int32 align;\n"
    "    Capability[] caps;\n"
    "    boolean needSlab;\n"
    "    public V(String n, Regime g, int32 f, int32 p, int32 a,\n"
    "             Capability[] c, boolean ns) {\n"
    "        this.nm #= n;\n"
    "        this.rg = g;\n"
    "        this.fmt = f;\n"
    "        this.pri = p;\n"
    "        this.align = a;\n"
    "        this.caps #= c;\n"
    "        this.needSlab = ns;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return this.rg; }\n"
    "    public int32 format() { return this.fmt; }\n"
    "    public int32 priority() { return this.pri; }\n"
    "    public String shapeRefusal(RouteQuery q) {\n"
    "        if (q instanceof WQuery w) {\n"
    "            if (w.inDim % this.align != 0) { return \"width\"; }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a weight query\";\n"
    "    }\n"
    "    public Capability[] needs() { return this.caps; }\n"
    "    public String readyRefusal(RouteCall c) {\n"
    "        if (!this.needSlab) { return null; }\n"
    "        if (c instanceof WCall w) {\n"
    "            if (w.slabBound) { return null; }\n"
    "            return \"slab\";\n"
    "        }\n"
    "        return \"not a weight call\";\n"
    "    }\n"
    "    public void dispatch(RouteCall c) {\n"
    "        V.dispatched = V.dispatched + 1;\n"
    "        return;\n"
    "    }\n"
    "}\n"
    "\n";

// A program with no kernels bundles no backend: the compiler emits the
// `__cajeta_xpu_register_backend` ctor only for programs that have kernels, so
// `Device.activeBackend()` is "none" and every capability answers false. That
// is not a device saying no — it is no device at all, and a capability test run
// against it exercises one half of its predicate and calls it covered.
//
// One trivial kernel gives the program a device. Only the capability test needs
// it; the rest of the suite is about shapes and states, not hardware.
const char* kKernelSoABackendBundles =
    "public final class K {\n"
    "    @Kernel\n"
    "    public static void noop(uint32 i, float32[] y, uint32 n) {\n"
    "        if (i < n) { y[i] = y[i]; }\n"
    "        return;\n"
    "    }\n"
    "}\n"
    "\n";

int runWeights(const std::string& body, const char* extra = "") {
    std::string program = std::string(kWeightRegistrant) + extra +
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

} // namespace

// 1.1.1 — Q4_K's wave variant and its packed variant are two rows of the same
// format. Priority decides, and the shadowed one is countable (spec §3.4.6).
TEST(XpuRouteTable, priorityDecidesBetweenTwoVariantsOfOneFormat) {
    int rc = runWeights(
        "        V packed = heap V(\"q4k-packed\", Regime.DecodeRow, 12, 10,\n"
        "            256, null, false);\n"
        "        V wave = heap V(\"q4k-wave\", Regime.DecodeRow, 12, 20,\n"
        "            256, null, false);\n"
        "        t.register(packed);\n"
        "        t.register(wave);\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        Route got = t.admissible(q);\n"
        "        if (got == null) { return 1; }\n"
        "        if (!got.name().equals(\"q4k-wave\")) { return 2; }\n"
        "        if (t.admitCount(q) != 2) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.2 — nothing serving the format returns nothing, and NOT the last row
// consulted: that fallthrough is what sent codebook bytes through the Q6_K
// decoder, wrong logits and no crash (spec §1.1).
TEST(XpuRouteTable, refusesRatherThanReturningTheLastRowConsulted) {
    int rc = runWeights(
        "        t.register(heap V(\"a\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"b\", Regime.DecodeRow, 14, 20, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 23, 1024);\n"
        "        if (t.admissible(q) != null) { return 1; }\n"
        "        if (t.admitCount(q) != 0) { return 2; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.2b — a shape the row does not fit is refused on the STATIC half, with
// nothing bound. 1408 is the routed down projection's width and is not a
// multiple of 256; that is a real refusal, not a synthetic one.
TEST(XpuRouteTable, aShapeTheRowDoesNotFitIsRefusedStatically) {
    int rc = runWeights(
        "        t.register(heap V(\"wave256\", Regime.DecodeRow, 12, 20,\n"
        "            256, null, false));\n"
        "        t.register(heap V(\"wave32\", Regime.DecodeRow, 12, 10,\n"
        "            32, null, false));\n"
        "        WQuery wide = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
        "        Route a = t.admissible(wide);\n"
        "        if (a == null || !a.name().equals(\"wave256\")) { return 1; }\n"
        "        WQuery odd = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        Route b = t.admissible(odd);\n"
        "        if (b == null || !b.name().equals(\"wave32\")) { return 2; }\n"
        "        if (t.admitCount(odd) != 1) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.3 — the regime is part of the key.
TEST(XpuRouteTable, aRegimeIsPartOfTheKey) {
    int rc = runWeights(
        "        t.register(heap V(\"dec\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"pre\", Regime.PrefillBatch, 12, 10, 256,\n"
        "            null, false));\n"
        "        WQuery d = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        WQuery p = heap WQuery(Regime.PrefillBatch, 12, 1024);\n"
        "        Route g1 = t.admissible(d);\n"
        "        Route g2 = t.admissible(p);\n"
        "        if (g1 == null || !g1.name().equals(\"dec\")) { return 1; }\n"
        "        if (g2 == null || !g2.name().equals(\"pre\")) { return 2; }\n"
        "        if (t.admitCount(d) != 1) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.4 — `needs` is checked against the ACTIVE device, in BOTH directions,
// and the fire half is the point.
//
// The program carries a kernel, so a backend bundles and the device has an
// identity: on the CPU backend `AtomicInt64` and `CoopMatrixBf16F32Acc` answer
// true while the two ray-query capabilities answer false. The assertion is the
// biconditional against whatever the device actually says, so it is correct on
// any backend — and the counters at the end FAIL the test if either half went
// unexercised, because a capability test that only ever sees "no" is the guard
// that skips nowhere.
//
// rc 3 means the backend cache was already set to "none" before this ran. The
// selection is per PROCESS and latches on first touch, so a capability test
// added to this suite without a kernel of its own would poison this one.
TEST(XpuRouteTable, aRowIsTakenExactlyWhenTheDeviceHasWhatItNeeds) {
    int rc = runWeights(
        "        String be #= Device.activeBackend();\n"
        "        if (be.equals(\"none\")) { return 3; }\n"
        "        Capability[] a = heap Capability[1];\n"
        "        a[0] = Capability.AtomicInt64;\n"
        "        Capability[] cm = heap Capability[1];\n"
        "        cm[0] = Capability.CoopMatrixBf16F32Acc;\n"
        "        Capability[] rq = heap Capability[1];\n"
        "        rq[0] = Capability.RayQueryNative;\n"
        "        Capability[] rt = heap Capability[1];\n"
        "        rt[0] = Capability.RayQueryRtCore;\n"
        "        Capability[] none = heap Capability[0];\n"
        "\n"
        "        int32 fired = 0;\n"
        "        int32 refused = 0;\n"
        "        int32 k = 0;\n"
        "        while (k < 5) {\n"
        "            Capability[] caps = none;\n"
        "            boolean want = true;\n"
        "            if (k == 1) { caps = a; }\n"
        "            if (k == 2) { caps = cm; }\n"
        "            if (k == 3) { caps = rq; }\n"
        "            if (k == 4) { caps = rt; }\n"
        "            if (k == 1) { want = Device.supports(Capability.AtomicInt64); }\n"
        "            if (k == 2) {\n"
        "                want = Device.supports(Capability.CoopMatrixBf16F32Acc);\n"
        "            }\n"
        "            if (k == 3) { want = Device.supports(Capability.RayQueryNative); }\n"
        "            if (k == 4) { want = Device.supports(Capability.RayQueryRtCore); }\n"
        "            RouteTable one = heap RouteTable();\n"
        "            one.register(heap V(\"row\", Regime.DecodeRow, 12, 10,\n"
        "                256, caps, false));\n"
        "            WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "            boolean got = one.admissible(q) != null;\n"
        "            if (got != want) { return 10 + k; }\n"
        "            if (one.admitCount(q) != (want ? 1 : 0)) { return 20 + k; }\n"
        "            if (k > 0) {\n"
        "                if (want) { fired = fired + 1; }\n"
        "                if (!want) { refused = refused + 1; }\n"
        "            }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        if (fired == 0) { return 1; }\n"
        "        if (refused == 0) { return 2; }\n"
        "        if (be.equals(\"none\")) { return 3; }\n"
        "        return 0;\n",
        kKernelSoABackendBundles);
    EXPECT_EQ(rc, 0);
}


// 1.1.5 — registration order does not reach the answer.
TEST(XpuRouteTable, registrationOrderDoesNotChangeTheAnswer) {
    int rc = runWeights(
        "        t.register(heap V(\"hi\", Regime.DecodeRow, 12, 20, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"lo\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        RouteTable t2 = heap RouteTable();\n"
        "        t2.register(heap V(\"lo\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        t2.register(heap V(\"hi\", Regime.DecodeRow, 12, 20, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        Route g1 = t.admissible(q);\n"
        "        Route g2 = t2.admissible(q);\n"
        "        if (g1 == null || g2 == null) { return 1; }\n"
        "        if (!g1.name().equals(g2.name())) { return 2; }\n"
        "        if (!g1.name().equals(\"hi\")) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.6 — THE SPLIT, and the unit's point. A row that fits but whose slab has
// not bound is ADMISSIBLE and not PICKED. `ExpertBank.idReady` answers both
// questions at once today, which is why widening it alone would have fed
// codebook bytes to a kernel that cannot read them.
TEST(XpuRouteTable, aRowThatFitsButIsNotReadyIsAdmissibleAndNotPicked) {
    int rc = runWeights(
        "        t.register(heap V(\"slab\", Regime.DecodeRow, 12, 30, 256,\n"
        "            null, true));\n"
        "        t.register(heap V(\"host\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        Route stat = t.admissible(q);\n"
        "        if (stat == null || !stat.name().equals(\"slab\")) { return 1; }\n"
        "        if (t.admitCount(q) != 2) { return 2; }\n"
        "        WCall unbound = heap WCall(q, false);\n"
        "        Route a = t.pick(unbound);\n"
        "        if (a == null || !a.name().equals(\"host\")) { return 3; }\n"
        "        WCall bound = heap WCall(q, true);\n"
        "        Route b = t.pick(bound);\n"
        "        if (b == null || !b.name().equals(\"slab\")) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.7 — the static half never reads runtime state. A row whose `ready` reads
// `slabBound` gives the SAME admissible answer for both bindings, because the
// query it is asked with carries no receiver to read it through.
TEST(XpuRouteTable, theStaticHalfCannotSeeRuntimeState) {
    int rc = runWeights(
        "        t.register(heap V(\"slab\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, true));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        Route s1 = t.admissible(q);\n"
        "        if (s1 == null) { return 1; }\n"
        "        WCall unbound = heap WCall(q, false);\n"
        "        if (t.pick(unbound) != null) { return 2; }\n"
        "        Route s2 = t.admissible(q);\n"
        "        if (s2 == null) { return 3; }\n"
        "        if (!s1.name().equals(s2.name())) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.9 — resolution is a bind-time call, but it still must not allocate: a
// registrant re-resolving on a rebind should not churn. The control is chosen
// rather than assumed — `Cajeta.allocatedBytes()` does not see a small
// non-escaping class or an array, so the obvious control would read flat for
// the same reason a broken selector would.
TEST(XpuRouteTable, resolutionAllocatesNothingBeyondTheQuery) {
    int rc = runWeights(
        "        t.register(heap V(\"a\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        WCall c = heap WCall(q, true);\n"
        "        Route warm = t.pick(c);\n"
        "        if (warm == null) { return 1; }\n"
        "        int64 b0 = Cajeta.allocatedBytes();\n"
        "        int32 k = 0;\n"
        "        while (k < 256) {\n"
        "            Route got = t.pick(c);\n"
        "            if (got == null) { return 2; }\n"
        "            if (t.admissible(q) == null) { return 3; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        int64 b1 = Cajeta.allocatedBytes();\n"
        "        if (b1 != b0) { return 4; }\n"
        "        RouteTable other = heap RouteTable();\n"
        "        if (other.count() != 0) { return 5; }\n"
        "        int64 b2 = Cajeta.allocatedBytes();\n"
        "        if (b2 <= b1) { return 6; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// A row dispatches without choosing: one variant, one kernel, no arm to omit.
TEST(XpuRouteTable, aPickedRowDispatchesWithoutChoosing) {
    int rc = runWeights(
        "        t.register(heap V(\"only\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        WCall c = heap WCall(q, true);\n"
        "        V.dispatched = 0;\n"
        "        Route got = t.pick(c);\n"
        "        if (got == null) { return 1; }\n"
        "        got.dispatch(c);\n"
        "        got.dispatch(c);\n"
        "        if (V.dispatched != 2) { return 2; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// The candidate set, ordered — what a measured selector ranks when `priority`
// is only a declared default. On hardware nobody has measured, that default is
// a guess, so the set has to be reachable and not just its first element.
TEST(XpuRouteTable, theAdmissibleSetIsEnumerableHighestPriorityFirst) {
    int rc = runWeights(
        "        t.register(heap V(\"mid\", Regime.DecodeRow, 12, 20, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"low\", Regime.DecodeRow, 12, 10, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"top\", Regime.DecodeRow, 12, 30, 256,\n"
        "            null, false));\n"
        "        t.register(heap V(\"other\", Regime.DecodeRow, 14, 99, 256,\n"
        "            null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        Route[] out = heap Route[4];\n"
        "        int32 n = t.candidates(q, out);\n"
        "        if (n != 3) { return 1; }\n"
        "        if (!out[0].name().equals(\"top\")) { return 2; }\n"
        "        if (!out[1].name().equals(\"mid\")) { return 3; }\n"
        "        if (!out[2].name().equals(\"low\")) { return 4; }\n"
        "        Route[] small = heap Route[2];\n"
        "        int32 m = t.candidates(q, small);\n"
        "        if (m != 3) { return 5; }\n"
        "        if (!small[0].name().equals(\"top\")) { return 6; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}
