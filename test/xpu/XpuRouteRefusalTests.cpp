//
// XpuRouteRefusalTests — what a row says when it does NOT take the work.
//
// The failure the table exists to stop was not that a route refused; it was
// that a refusal was silent and reading a census took bisection (route-table
// spec §1.1, §3.4.4). So a refusal is an object: the row that came closest,
// the clause that stopped it, and the row's OWN word for the gate — because
// `zeroSyncReady` names nine distinct gates today and the table must not
// coarsen that to "not ready".
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {

// A registrant with TWO static gates and TWO runtime gates on one row, which
// is the shape `zeroSyncReady` actually has: four shape tests and two slab
// tests, each with its own sentence in the `moe-row-route` record.
const char* kGatedRegistrant =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Capability;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.Regime;\n"
    "import cajeta.xpu.Route;\n"
    "import cajeta.xpu.RouteCall;\n"
    "import cajeta.xpu.RouteClause;\n"
    "import cajeta.xpu.RouteQuery;\n"
    "import cajeta.xpu.RouteRefusal;\n"
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
    "    public boolean widened;\n"
    "    public WCall(WQuery q, boolean bound, boolean widened) {\n"
    "        this.query #= q;\n"
    "        this.slabBound = bound;\n"
    "        this.widened = widened;\n"
    "    }\n"
    "}\n"
    "\n"
    "public final class V implements Route {\n"
    "    String nm;\n"
    "    Regime rg;\n"
    "    int32 fmt;\n"
    "    int32 pri;\n"
    "    int32 align;\n"
    "    int32 minDim;\n"
    "    Capability[] caps;\n"
    "    boolean gates;\n"
    "    public V(String n, Regime g, int32 f, int32 p, int32 a, int32 md,\n"
    "             Capability[] c, boolean gates) {\n"
    "        this.nm #= n;\n"
    "        this.rg = g;\n"
    "        this.fmt = f;\n"
    "        this.pri = p;\n"
    "        this.align = a;\n"
    "        this.minDim = md;\n"
    "        this.caps #= c;\n"
    "        this.gates = gates;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return this.rg; }\n"
    "    public int32 format() { return this.fmt; }\n"
    "    public int32 priority() { return this.pri; }\n"
    "    public String shapeRefusal(RouteQuery q) {\n"
    "        if (q instanceof WQuery w) {\n"
    "            if (w.inDim < this.minDim) { return \"below min width\"; }\n"
    "            if (w.inDim % this.align != 0) { return \"width not aligned\"; }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a weight query\";\n"
    "    }\n"
    "    public Capability[] needs() { return this.caps; }\n"
    "    public String readyRefusal(RouteCall c) {\n"
    "        if (!this.gates) { return null; }\n"
    "        if (c instanceof WCall w) {\n"
    "            if (!w.slabBound) { return \"slab is not bound\"; }\n"
    "            if (!w.widened) { return \"slab is not widened\"; }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a weight call\";\n"
    "    }\n"
    "    public void dispatch(RouteCall c) { return; }\n"
    "}\n"
    "\n";

int runGated(const std::string& body) {
    std::string program = std::string(kGatedRegistrant) +
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

// 2.1.1 — a refusal names the row and the clause, and the clauses are
// DISTINGUISHABLE by a test rather than by reading a string. Four of them here
// in one table: a format nothing serves, a shape that does not fit, a
// capability the device lacks, and a receiver that is not ready.
TEST(XpuRouteRefusal, theClausesAreDistinguishableByTest) {
    int rc = runGated(
        "        Capability[] rt = heap Capability[1];\n"
        "        rt[0] = Capability.RayQueryRtCore;\n"
        "        t.register(heap V(\"aligned\", Regime.DecodeRow, 12, 10,\n"
        "            256, 0, null, false));\n"
        "        t.register(heap V(\"needs-rt\", Regime.DecodeRow, 14, 10,\n"
        "            1, 0, rt, false));\n"
        "        t.register(heap V(\"slabbed\", Regime.DecodeRow, 15, 10,\n"
        "            1, 0, null, true));\n"
        "\n"
        "        WQuery unknown = heap WQuery(Regime.DecodeRow, 23, 1024);\n"
        "        RouteRefusal f #= t.whyNotAdmissible(unknown);\n"
        "        if (f == null) { return 1; }\n"
        "        if (f.clause != RouteClause.Format) { return 2; }\n"
        "\n"
        "        WQuery odd = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        RouteRefusal s #= t.whyNotAdmissible(odd);\n"
        "        if (s == null) { return 3; }\n"
        "        if (s.clause != RouteClause.Shape) { return 4; }\n"
        "        if (s.row == null) { return 5; }\n"
        "        if (!s.row.name().equals(\"aligned\")) { return 6; }\n"
        "        if (!s.detail.equals(\"width not aligned\")) { return 7; }\n"
        "\n"
        "        WQuery capq = heap WQuery(Regime.DecodeRow, 14, 1024);\n"
        "        boolean has = Device.supports(Capability.RayQueryRtCore);\n"
        "        RouteRefusal u #= t.whyNotAdmissible(capq);\n"
        "        if (!has) {\n"
        "            if (u == null) { return 8; }\n"
        "            if (u.clause != RouteClause.Unsupported) { return 9; }\n"
        "            if (u.needIndex != 0) { return 10; }\n"
        "            if (u.row == null) { return 11; }\n"
        "            if (!u.row.name().equals(\"needs-rt\")) { return 12; }\n"
        "        }\n"
        "\n"
        "        WQuery rq = heap WQuery(Regime.DecodeRow, 15, 1024);\n"
        "        if (t.whyNotAdmissible(rq) != null) { return 13; }\n"
        "        WCall unbound = heap WCall(rq, false, false);\n"
        "        RouteRefusal r #= t.whyNotPicked(unbound);\n"
        "        if (r == null) { return 14; }\n"
        "        if (r.clause != RouteClause.Ready) { return 15; }\n"
        "\n"
        "        WQuery wrongRegime = heap WQuery(Regime.PrefillBatch, 12, 1024);\n"
        "        RouteRefusal g #= t.whyNotAdmissible(wrongRegime);\n"
        "        if (g == null) { return 16; }\n"
        "        if (g.clause != RouteClause.Regime) { return 17; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 2.1.2 — an empty table says SO. Naming nothing at all is the silent refusal
// this unit exists to remove, and it is exactly the state a package is in
// before its rows are registered.
TEST(XpuRouteRefusal, anEmptyTableSaysSoRatherThanNamingNothing) {
    int rc = runGated(
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        RouteRefusal f #= t.whyNotAdmissible(q);\n"
        "        if (f == null) { return 1; }\n"
        "        if (f.clause != RouteClause.Empty) { return 2; }\n"
        "        if (f.row != null) { return 3; }\n"
        "        if (f.ty != 12) { return 4; }\n"
        "        if (f.regime != Regime.DecodeRow) { return 5; }\n"
        "        String line #= f.text();\n"
        "        if (!line.contains(\"no rows\")) { return 6; }\n"
        "        WCall c = heap WCall(q, true, true);\n"
        "        RouteRefusal p #= t.whyNotPicked(c);\n"
        "        if (p == null) { return 7; }\n"
        "        if (p.clause != RouteClause.Empty) { return 8; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 2.1.3 — the clause a `ready` refusal reports is the ROW's OWN. One row, two
// runtime gates, and the refusal distinguishes them: `zeroSyncReady` names
// nine such gates today and the table replacing it must not coarsen them into
// a single "not ready".
TEST(XpuRouteRefusal, aReadyRefusalCarriesTheRowsOwnGate) {
    int rc = runGated(
        "        t.register(heap V(\"row\", Regime.DecodeRow, 12, 10, 1, 0,\n"
        "            null, true));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        RouteRefusal a #= t.whyNotPicked(heap WCall(q, false, false));\n"
        "        if (a == null) { return 1; }\n"
        "        if (a.clause != RouteClause.Ready) { return 2; }\n"
        "        if (!a.detail.equals(\"slab is not bound\")) { return 3; }\n"
        "        RouteRefusal b #= t.whyNotPicked(heap WCall(q, true, false));\n"
        "        if (b == null) { return 4; }\n"
        "        if (b.clause != RouteClause.Ready) { return 5; }\n"
        "        if (!b.detail.equals(\"slab is not widened\")) { return 6; }\n"
        "        if (a.detail.equals(b.detail)) { return 7; }\n"
        "        if (t.whyNotPicked(heap WCall(q, true, true)) != null) {\n"
        "            return 8;\n"
        "        }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 2.3.2 — and the STATIC half is held to the same standard. Four of
// `zeroSyncReady`'s gates are shape tests, so a row's `shapeRefusal` names
// which one refused rather than answering a bare false.
TEST(XpuRouteRefusal, aShapeRefusalCarriesTheRowsOwnGate) {
    int rc = runGated(
        "        t.register(heap V(\"row\", Regime.DecodeRow, 12, 10, 256,\n"
        "            512, null, false));\n"
        "        WQuery small = heap WQuery(Regime.DecodeRow, 12, 256);\n"
        "        RouteRefusal a #= t.whyNotAdmissible(small);\n"
        "        if (a == null) { return 1; }\n"
        "        if (a.clause != RouteClause.Shape) { return 2; }\n"
        "        if (!a.detail.equals(\"below min width\")) { return 3; }\n"
        "        WQuery odd = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        RouteRefusal b #= t.whyNotAdmissible(odd);\n"
        "        if (b == null) { return 4; }\n"
        "        if (!b.detail.equals(\"width not aligned\")) { return 5; }\n"
        "        if (a.detail.equals(b.detail)) { return 6; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// The refusal and the selector answer the same question. A refusal exists
// EXACTLY when no row was taken — asserted over a matrix rather than at one
// point, because a refusal that disagreed with the selector would read as a
// clean run while pointing at the wrong row.
TEST(XpuRouteRefusal, aRefusalExistsExactlyWhenNoRowIsTaken) {
    int rc = runGated(
        "        t.register(heap V(\"row\", Regime.DecodeRow, 12, 10, 256,\n"
        "            512, null, true));\n"
        "        int32 ty = 12;\n"
        "        while (ty <= 13) {\n"
        "            int32 d = 256;\n"
        "            while (d <= 1024) {\n"
        "                WQuery q = heap WQuery(Regime.DecodeRow, ty, d);\n"
        "                boolean adm = t.admissible(q) != null;\n"
        "                boolean why = t.whyNotAdmissible(q) == null;\n"
        "                if (adm != why) { return 1; }\n"
        "                int32 bits = 0;\n"
        "                while (bits < 4) {\n"
        "                    WCall c = heap WCall(q, bits % 2 == 1,\n"
        "                        bits / 2 == 1);\n"
        "                    boolean got = t.pick(c) != null;\n"
        "                    boolean none = t.whyNotPicked(c) == null;\n"
        "                    if (got != none) { return 2; }\n"
        "                    bits = bits + 1;\n"
        "                }\n"
        "                d = d + 256;\n"
        "            }\n"
        "            ty = ty + 1;\n"
        "        }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// The refusal names the row that got FURTHEST, not the last one consulted.
// Unit 1 settled that registration order does not reach the answer; a
// diagnostic that named whichever row happened to be registered last would put
// that order straight back, in the one place a reader trusts it.
TEST(XpuRouteRefusal, theRowNamedIsTheOneThatGotFurthestNotTheLastConsulted) {
    int rc = runGated(
        "        t.register(heap V(\"close\", Regime.DecodeRow, 12, 10, 256,\n"
        "            0, null, false));\n"
        "        t.register(heap V(\"far\", Regime.DecodeRow, 99, 90, 1,\n"
        "            0, null, false));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        RouteRefusal a #= t.whyNotAdmissible(q);\n"
        "        if (a == null || a.row == null) { return 1; }\n"
        "        if (!a.row.name().equals(\"close\")) { return 2; }\n"
        "        RouteTable t2 = heap RouteTable();\n"
        "        t2.register(heap V(\"far\", Regime.DecodeRow, 99, 90, 1,\n"
        "            0, null, false));\n"
        "        t2.register(heap V(\"close\", Regime.DecodeRow, 12, 10, 256,\n"
        "            0, null, false));\n"
        "        RouteRefusal b #= t2.whyNotAdmissible(q);\n"
        "        if (b == null || b.row == null) { return 3; }\n"
        "        if (!b.row.name().equals(\"close\")) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 2.2.3 / 2.3.1 — the one-line form a diagnostic record carries. It names the
// row and the gate in one step; the failure mode of spec §1.1 is that reading
// a census for the same fact took bisection.
TEST(XpuRouteRefusal, theOneLineFormNamesTheRowAndTheGate) {
    int rc = runGated(
        "        t.register(heap V(\"iq3xxs-id-row\", Regime.DecodeRow, 12,\n"
        "            10, 256, 0, null, true));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        RouteRefusal s #= t.whyNotAdmissible(q);\n"
        "        if (s == null) { return 1; }\n"
        "        String a #= s.text();\n"
        "        if (!a.contains(\"iq3xxs-id-row\")) { return 2; }\n"
        "        if (!a.contains(\"width not aligned\")) { return 3; }\n"
        "        if (!a.contains(\"shape\")) { return 4; }\n"
        "        if (!a.contains(\"DecodeRow\")) { return 5; }\n"
        "        WQuery ok = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        RouteRefusal r #= t.whyNotPicked(heap WCall(ok, false, false));\n"
        "        if (r == null) { return 6; }\n"
        "        String b #= r.text();\n"
        "        if (!b.contains(\"iq3xxs-id-row\")) { return 7; }\n"
        "        if (!b.contains(\"slab is not bound\")) { return 8; }\n"
        "        if (!b.contains(\"ready\")) { return 9; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// A refusal is built only when one is ASKED for. Resolution stays the
// allocation-free bind-time call Unit 1 fixed it as: the cost of naming a
// refusal is paid on the path that is about to print something, never on the
// path that took the row.
TEST(XpuRouteRefusal, namingARefusalCostsNothingOnTheTakenPath) {
    int rc = runGated(
        "        t.register(heap V(\"row\", Regime.DecodeRow, 12, 10, 256,\n"
        "            0, null, true));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 1024);\n"
        "        WCall c = heap WCall(q, true, true);\n"
        "        if (t.pick(c) == null) { return 1; }\n"
        "        int64 b0 = Cajeta.allocatedBytes();\n"
        "        int32 k = 0;\n"
        "        while (k < 256) {\n"
        "            if (t.pick(c) == null) { return 2; }\n"
        "            if (t.whyNotPicked(c) != null) { return 3; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        int64 b1 = Cajeta.allocatedBytes();\n"
        "        if (b1 != b0) { return 4; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}
