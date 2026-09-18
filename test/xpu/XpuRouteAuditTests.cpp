//
// XpuRouteAuditTests — the walk that says what the table does NOT serve.
//
// `theFourPartInvariant` covers four gates and has held since it was written;
// the routes grew to a dozen predicates outside it, so it stayed green while a
// route it did not know refused (route-table spec §1.1). The audit walks the
// TABLE, so a row that exists is a row it sees, and it walks the static half
// only — with nothing bound and no device, which is what makes it a host test.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {

// The registrant's own format set and its own queries: the table knows a
// regime and an opaque `ty`, and nothing about what a weight is.
//
// `readyRefusal` COUNTS its calls and answers a gate that would fail every
// assertion here. The audit must never reach it — if it did, the walk would
// depend on a bound weight and could not be a host test at all.
const char* kAuditRegistrant =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Capability;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.Regime;\n"
    "import cajeta.xpu.Route;\n"
    "import cajeta.xpu.RouteAudit;\n"
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
    "public final class V implements Route {\n"
    "    public static int32 readyCalls;\n"
    "    String nm;\n"
    "    Regime rg;\n"
    "    int32 fmt;\n"
    "    int32 pri;\n"
    "    int32 align;\n"
    "    Capability[] caps;\n"
    "    public V(String n, Regime g, int32 f, int32 p, int32 a) {\n"
    "        this.nm #= n;\n"
    "        this.rg = g;\n"
    "        this.fmt = f;\n"
    "        this.pri = p;\n"
    "        this.align = a;\n"
    "        this.caps = null;\n"
    "    }\n"
    "    public V needing(Capability[] c) {\n"
    "        this.caps #= c;\n"
    "        return this;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return this.rg; }\n"
    "    public int32 format() { return this.fmt; }\n"
    "    public int32 priority() { return this.pri; }\n"
    "    public String shapeRefusal(RouteQuery q) {\n"
    "        if (q instanceof WQuery w) {\n"
    "            if (w.inDim % this.align != 0) { return \"width not aligned\"; }\n"
    "            return null;\n"
    "        }\n"
    "        return \"not a weight query\";\n"
    "    }\n"
    "    public Capability[] needs() { return this.caps; }\n"
    "    public String readyRefusal(RouteCall c) {\n"
    "        V.readyCalls = V.readyCalls + 1;\n"
    "        return \"the audit reached bound state\";\n"
    "    }\n"
    "    public void dispatch(RouteCall c) { return; }\n"
    "}\n"
    "\n"
    "public final class Bank {\n"
    "    public static #RouteTable table() {\n"
    "        RouteTable t = heap RouteTable();\n"
    "        t.register(heap V(\"q4k-wave\", Regime.DecodeRow, 12, 20, 256));\n"
    "        t.register(heap V(\"q4k-packed\", Regime.DecodeRow, 12, 10, 1));\n"
    "        t.register(heap V(\"iq4nl-wave\", Regime.DecodeRow, 20, 10, 32));\n"
    "        t.register(heap V(\"mxfp4-tile\", Regime.PrefillBatch, 30, 10, 1));\n"
    "        return #t;\n"
    "    }\n"
    "    public static #RouteQuery[] queries() {\n"
    "        RouteQuery[] qs = heap RouteQuery[4];\n"
    "        qs[0] = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
    "        qs[1] = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
    "        qs[2] = heap WQuery(Regime.DecodeRow, 20, 1408);\n"
    "        qs[3] = heap WQuery(Regime.DecodeRow, 23, 2048);\n"
    "        return #qs;\n"
    "    }\n"
    "}\n"
    "\n";

int runAudit(const std::string& body) {
    std::string program = std::string(kAuditRegistrant) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        V.readyCalls = 0;\n"
        "        RouteTable t #= Bank.table();\n"
        "        RouteQuery[] qs #= Bank.queries();\n"
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

// 3.1.1 — per (row, query), admitted or refused BY NAME. The cell is a clause,
// not a boolean, so "this row is for another format" and "this row is for this
// format and your width does not fit" are different answers.
TEST(XpuRouteAudit, everyRowAnswersForEveryQueryAndTheAnswerIsNamed) {
    int rc = runAudit(
        "        RouteAudit a #= t.audit(qs);\n"
        "        if (a.queryCount() != 4) { return 1; }\n"
        "        if (a.rowCount() != 4) { return 2; }\n"
        "        if (!a.rowAt(0).name().equals(\"q4k-wave\")) { return 3; }\n"
        "        if (a.clauseAt(0, 0) != RouteClause.Taken) { return 4; }\n"
        "        if (a.clauseAt(1, 0) != RouteClause.Shape) { return 5; }\n"
        "        if (a.clauseAt(2, 0) != RouteClause.Format) { return 6; }\n"
        "        if (a.clauseAt(0, 1) != RouteClause.Taken) { return 7; }\n"
        "        if (a.clauseAt(1, 1) != RouteClause.Taken) { return 8; }\n"
        "        if (a.clauseAt(2, 2) != RouteClause.Taken) { return 9; }\n"
        "        if (a.clauseAt(0, 3) != RouteClause.Format) { return 10; }\n"
        "        if (!a.admits(0, 0)) { return 11; }\n"
        "        if (a.admits(1, 0)) { return 12; }\n"
        "        int32 qi = 0;\n"
        "        while (qi < a.queryCount()) {\n"
        "            int32 ri = 0;\n"
        "            while (ri < a.rowCount()) {\n"
        "                if (a.clauseAt(qi, ri) == RouteClause.Empty) {\n"
        "                    return 13;\n"
        "                }\n"
        "                if (a.rowAt(ri).name().byteLength() == 0L) {\n"
        "                    return 14;\n"
        "                }\n"
        "                ri = ri + 1;\n"
        "            }\n"
        "            qi = qi + 1;\n"
        "        }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 3.1.2 — a format that arrives after the rows were written. Every row is
// named against it, nothing is bound, and no device was asked: this is the
// check that would have caught `idReady`'s list of types 12 and 14 the day
// the codebook banks landed.
TEST(XpuRouteAudit, aNewFormatNamesEveryRowThatLacksIt) {
    int rc = runAudit(
        "        RouteAudit a #= t.audit(qs);\n"
        "        if (a.servedBy(3) != 0) { return 1; }\n"
        "        if (!a.unserved(3)) { return 2; }\n"
        "        if (a.unservedCount() != 1) { return 3; }\n"
        "        if (a.firstUnserved() != 3) { return 4; }\n"
        "        int32 ri = 0;\n"
        "        while (ri < a.rowCount()) {\n"
        "            if (a.admits(3, ri)) { return 5; }\n"
        "            ri = ri + 1;\n"
        "        }\n"
        "        String rep #= a.report();\n"
        "        if (!rep.contains(\"q4k-wave\")) { return 6; }\n"
        "        if (!rep.contains(\"q4k-packed\")) { return 7; }\n"
        "        if (!rep.contains(\"iq4nl-wave\")) { return 8; }\n"
        "        if (!rep.contains(\"mxfp4-tile\")) { return 9; }\n"
        "        if (!rep.contains(\"ty 23\")) { return 10; }\n"
        "        if (!rep.contains(\"unserved\")) { return 11; }\n"
        "        if (a.servedBy(0) == 0) { return 12; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 3.1.3 — two rows admitting one (format, regime) are REPORTED as a shadowed
// pair, not silently ordered. Priority still decides; the point is that the
// choice is visible, which is the seed of measured selection (spec §3.4.6).
TEST(XpuRouteAudit, twoRowsForOneFormatAreReportedAsShadowedNotSilentlyOrdered) {
    int rc = runAudit(
        "        RouteAudit a #= t.audit(qs);\n"
        "        if (a.servedBy(0) != 2) { return 1; }\n"
        "        if (!a.shadowed(0)) { return 2; }\n"
        "        if (a.shadowed(1)) { return 3; }\n"
        "        if (a.shadowedCount() != 1) { return 4; }\n"
        "        if (a.firstShadowed() != 0) { return 5; }\n"
        "        String rep #= a.report();\n"
        "        if (!rep.contains(\"shadowed\")) { return 6; }\n"
        "        Route taken = t.admissible(qs[0]);\n"
        "        if (taken == null) { return 7; }\n"
        "        if (!taken.name().equals(\"q4k-wave\")) { return 8; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 3.1.4 — the fire / no-fire pair, per row. A predicate that silently disabled
// a whole check read as a clean run for an hour (codebook-quants 9.2.7), so a
// row that admits everything it is shown, or nothing, is reported rather than
// counted as green.
TEST(XpuRouteAudit, aRowOwesADoesFireAndADoesNotFireAnswer) {
    int rc = runAudit(
        "        RouteQuery[] own = heap RouteQuery[3];\n"
        "        own[0] = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
        "        own[1] = heap WQuery(Regime.DecodeRow, 12, 1408);\n"
        "        own[2] = heap WQuery(Regime.DecodeRow, 12, 512);\n"
        "        Route wave = t.rowAt(0);\n"
        "        int32 fires = t.auditRow(wave, own);\n"
        "        if (fires != 2) { return 1; }\n"
        "        Route packed = t.rowAt(1);\n"
        "        if (t.auditRow(packed, own) != 3) { return 2; }\n"
        "        RouteAudit a #= t.audit(qs);\n"
        "        if (a.firesFor(0) != 1) { return 3; }\n"
        "        if (a.neverFires(0)) { return 4; }\n"
        "        if (!a.neverFires(3)) { return 5; }\n"
        "        if (a.neverFiringCount() != 1) { return 6; }\n"
        "        if (a.firstNeverFiring() != 3) { return 7; }\n"
        "        String rep #= a.report();\n"
        "        if (!rep.contains(\"never fires\")) { return 8; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 3.1.5 — the audit touches no bound state. The registrant's `readyRefusal`
// counts its calls and answers a gate that would fail every assertion above;
// an audit that reached it would need a bound weight and a live device, and
// could not run as a host test at all.
TEST(XpuRouteAudit, theAuditNeverReachesTheRuntimeHalf) {
    int rc = runAudit(
        "        RouteAudit a #= t.audit(qs);\n"
        "        if (a.queryCount() != 4) { return 1; }\n"
        "        String rep #= a.report();\n"
        "        if (rep.byteLength() == 0L) { return 2; }\n"
        "        RouteQuery[] own = heap RouteQuery[1];\n"
        "        own[0] = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
        "        if (t.auditRow(t.rowAt(0), own) != 1) { return 3; }\n"
        "        if (V.readyCalls != 0) { return 4; }\n"
        "        if (rep.contains(\"bound state\")) { return 5; }\n"
        "        RouteCall c = heap RouteCall(qs[0]);\n"
        "        if (t.pick(c) != null) { return 6; }\n"
        "        if (V.readyCalls == 0) { return 7; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 3.3.2 — the format set and the queries are the REGISTRANT's. The table is
// handed an array of its own query subclass and reads `regime` and `ty`; it
// names no format, no dimension and no quantization.
TEST(XpuRouteAudit, theFormatSetAndTheQueriesBelongToTheRegistrant) {
    int rc = runAudit(
        "        RouteAudit a #= t.audit(qs);\n"
        "        int32 qi = 0;\n"
        "        while (qi < a.queryCount()) {\n"
        "            RouteQuery q = a.queryAt(qi);\n"
        "            boolean mine = false;\n"
        "            if (q instanceof WQuery w) { mine = w.inDim > 0; }\n"
        "            if (!mine) { return 1; }\n"
        "            if (q.ty != qs[qi].ty) { return 2; }\n"
        "            qi = qi + 1;\n"
        "        }\n"
        "        RouteQuery[] empty = heap RouteQuery[0];\n"
        "        RouteAudit b #= t.audit(empty);\n"
        "        if (b.queryCount() != 0) { return 3; }\n"
        "        if (b.rowCount() != 4) { return 4; }\n"
        "        if (b.neverFiringCount() != 4) { return 5; }\n"
        "        if (b.unservedCount() != 0) { return 6; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// The audit's answer does not depend on the DEVICE, and this is the point of
// keeping `needs` out of its verdict. A capability is a fact about the box;
// whether the table has a row for a format is not. An audit that conflated
// them would report every row unserved on a machine without the part — which
// is to say, on the machine where you are most likely to be adding a format.
TEST(XpuRouteAudit, theAuditsAnswerDoesNotDependOnTheDevice) {
    int rc = runAudit(
        "        Capability[] rt = heap Capability[1];\n"
        "        rt[0] = Capability.RayQueryRtCore;\n"
        "        RouteTable u = heap RouteTable();\n"
        "        u.register(heap V(\"needs-rt\", Regime.DecodeRow, 12, 20,\n"
        "            256).needing(rt));\n"
        "        WQuery q = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
        "        RouteQuery[] one = heap RouteQuery[1];\n"
        "        one[0] = q;\n"
        "\n"
        "        if (Device.supports(Capability.RayQueryRtCore)) { return 1; }\n"
        "        if (u.admissible(q) != null) { return 2; }\n"
        "\n"
        "        RouteAudit a #= u.audit(one);\n"
        "        if (a.clauseAt(0, 0) != RouteClause.Taken) { return 3; }\n"
        "        if (!a.admits(0, 0)) { return 4; }\n"
        "        if (a.servedBy(0) != 1) { return 5; }\n"
        "        if (a.unserved(0)) { return 6; }\n"
        "        if (a.neverFires(0)) { return 7; }\n"
        "        if (u.auditRow(u.rowAt(0), one) != 1) { return 8; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// What a row needs is REPORTED, not ruled on. Dropping it from the verdict
// must not drop it from the walk: "this format has a row, and that row wants
// AtomicInt64" is the answer a reader adding a format needs, and it is one
// they can get without the part in hand.
TEST(XpuRouteAudit, theAuditReportsWhatARowNeedsAsAFactNotAVerdict) {
    int rc = runAudit(
        "        Capability[] two = heap Capability[2];\n"
        "        two[0] = Capability.AtomicInt64;\n"
        "        two[1] = Capability.RayQueryRtCore;\n"
        "        RouteTable u = heap RouteTable();\n"
        "        u.register(heap V(\"bare\", Regime.DecodeRow, 12, 10, 1));\n"
        "        u.register(heap V(\"hungry\", Regime.DecodeRow, 12, 20,\n"
        "            1).needing(two));\n"
        "        RouteQuery[] one = heap RouteQuery[1];\n"
        "        one[0] = heap WQuery(Regime.DecodeRow, 12, 2048);\n"
        "        RouteAudit a #= u.audit(one);\n"
        "        if (a.needsCount(0) != 0) { return 1; }\n"
        "        if (a.needsCount(1) != 2) { return 2; }\n"
        "        if (a.needAt(1, 0) != Capability.AtomicInt64) { return 3; }\n"
        "        if (a.needAt(1, 1) != Capability.RayQueryRtCore) { return 4; }\n"
        "        String rep #= a.report();\n"
        "        if (!rep.contains(\"AtomicInt64\")) { return 5; }\n"
        "        if (!rep.contains(\"RayQueryRtCore\")) { return 6; }\n"
        "        if (!rep.contains(\"hungry\")) { return 7; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}
