//
// XpuRouteTableTests — the selection contract's rows and selector.
//
// A route table exists because the knowledge of which kernel serves which
// format kept being restated from memory, once per route, in whichever file
// the route lived (route-table spec §1.1). These tests hold the selector to
// the one behaviour that replaces those predicates: registration order does
// not matter, priority decides among rows that admit the same work, a regime
// is part of the key, and a row whose device cannot run it is not picked.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {

// One configurable row: it admits a single format, in a single regime, and
// reports whether its device is ready. Everything the selector does is a
// function of those, so one shape covers every test here.
const char* kRow =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Regime;\n"
    "import cajeta.xpu.Route;\n"
    "import cajeta.xpu.RouteCall;\n"
    "import cajeta.xpu.RouteShape;\n"
    "import cajeta.xpu.RouteTable;\n"
    "\n"
    "public final class R implements Route {\n"
    "    String nm;\n"
    "    Regime rg;\n"
    "    int32 pri;\n"
    "    int32 want;\n"
    "    boolean rdy;\n"
    "    public R(String n, Regime g, int32 p, int32 t, boolean r) {\n"
    "        this.nm #= n;\n"
    "        this.rg = g;\n"
    "        this.pri = p;\n"
    "        this.want = t;\n"
    "        this.rdy = r;\n"
    "    }\n"
    "    public String name() { return this.nm; }\n"
    "    public Regime regime() { return this.rg; }\n"
    "    public int32 priority() { return this.pri; }\n"
    "    public boolean admits(int32 ty, RouteShape s) {\n"
    "        return ty == this.want;\n"
    "    }\n"
    "    public boolean deviceReady() { return this.rdy; }\n"
    "    public boolean dispatch(RouteCall c) { return true; }\n"
    "}\n"
    "\n";

int runWith(const std::string& body) {
    std::string program = std::string(kRow) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        RouteTable t = heap RouteTable();\n"
        "        RouteShape s = heap RouteShape(1408, 2048, 1);\n"
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

// 1.1.1 — two rows admit the same (format, regime); the higher priority wins
// and the shadowed one is countable, which is the seed of measured selection
// (spec §3.4.6) without doing it yet.
TEST(XpuRouteTable, priorityDecidesAmongRowsThatAdmitTheSameWork) {
    int rc = runWith(
        "        R lo = heap R(\"lo\", Regime.DecodeRow, 10, 12, true);\n"
        "        R hi = heap R(\"hi\", Regime.DecodeRow, 20, 12, true);\n"
        "        t.register(lo);\n"
        "        t.register(hi);\n"
        "        Route got = t.pick(Regime.DecodeRow, 12, s);\n"
        "        if (got == null) { return 1; }\n"
        "        if (!got.name().equals(\"hi\")) { return 2; }\n"
        "        if (t.admitCount(Regime.DecodeRow, 12, s) != 2) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.2 — when nothing admits, the selector refuses. It must NOT hand back the
// last row consulted: that is the fallthrough that shipped codebook bytes into
// the Q6_K decoder, wrong logits and no crash (spec §1.1).
TEST(XpuRouteTable, refusesRatherThanReturningTheLastRowConsulted) {
    int rc = runWith(
        "        R a = heap R(\"a\", Regime.DecodeRow, 10, 12, true);\n"
        "        R b = heap R(\"b\", Regime.DecodeRow, 20, 14, true);\n"
        "        t.register(a);\n"
        "        t.register(b);\n"
        "        if (t.pick(Regime.DecodeRow, 23, s) != null) { return 1; }\n"
        "        if (t.admitCount(Regime.DecodeRow, 23, s) != 0) { return 2; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.3 — the regime is part of the key. A format served at decode and refused
// in the prefill batch is the ordinary case, not the exception.
TEST(XpuRouteTable, aRegimeIsPartOfTheKey) {
    int rc = runWith(
        "        R d = heap R(\"dec\", Regime.DecodeRow, 10, 12, true);\n"
        "        R p = heap R(\"pre\", Regime.PrefillBatch, 10, 14, true);\n"
        "        t.register(d);\n"
        "        t.register(p);\n"
        "        Route g1 = t.pick(Regime.DecodeRow, 12, s);\n"
        "        if (g1 == null) { return 1; }\n"
        "        if (!g1.name().equals(\"dec\")) { return 2; }\n"
        "        if (t.pick(Regime.PrefillBatch, 12, s) != null) { return 3; }\n"
        "        Route g2 = t.pick(Regime.PrefillBatch, 14, s);\n"
        "        if (g2 == null) { return 4; }\n"
        "        if (!g2.name().equals(\"pre\")) { return 5; }\n"
        "        if (t.pick(Regime.DecodeRow, 14, s) != null) { return 6; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.4 — a row the device cannot run is skipped even at the top priority, and
// a lower row takes the work. With no runnable row left, the table refuses.
TEST(XpuRouteTable, aRowTheDeviceCannotRunIsNotPicked) {
    int rc = runWith(
        "        R no = heap R(\"needs\", Regime.DecodeRow, 30, 12, false);\n"
        "        R ok = heap R(\"plain\", Regime.DecodeRow, 10, 12, true);\n"
        "        t.register(no);\n"
        "        t.register(ok);\n"
        "        Route got = t.pick(Regime.DecodeRow, 12, s);\n"
        "        if (got == null) { return 1; }\n"
        "        if (!got.name().equals(\"plain\")) { return 2; }\n"
        "        RouteTable t2 = heap RouteTable();\n"
        "        R only = heap R(\"only\", Regime.DecodeRow, 30, 12, false);\n"
        "        t2.register(only);\n"
        "        if (t2.pick(Regime.DecodeRow, 12, s) != null) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.1.5 — registration order does not reach the answer. Rows are declared in
// whatever order a package's files are walked, and the compiler's own source
// order is load-bearing elsewhere, so this is not a free property.
TEST(XpuRouteTable, registrationOrderDoesNotChangeTheAnswer) {
    int rc = runWith(
        "        R lo = heap R(\"lo\", Regime.DecodeRow, 10, 12, true);\n"
        "        R hi = heap R(\"hi\", Regime.DecodeRow, 20, 12, true);\n"
        "        t.register(hi);\n"
        "        t.register(lo);\n"
        "        Route g1 = t.pick(Regime.DecodeRow, 12, s);\n"
        "        RouteTable t2 = heap RouteTable();\n"
        "        R lo2 = heap R(\"lo\", Regime.DecodeRow, 10, 12, true);\n"
        "        R hi2 = heap R(\"hi\", Regime.DecodeRow, 20, 12, true);\n"
        "        t2.register(lo2);\n"
        "        t2.register(hi2);\n"
        "        Route g2 = t2.pick(Regime.DecodeRow, 12, s);\n"
        "        if (g1 == null || g2 == null) { return 1; }\n"
        "        if (!g1.name().equals(g2.name())) { return 2; }\n"
        "        if (!g1.name().equals(\"hi\")) { return 3; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

// 1.3.2 — `pick` is consulted per launch, so it must allocate nothing on the
// hit path. The second half is the control: the counter DOES move when
// something really allocates, so a flat reading is evidence and not a dead
// instrument. The control has to be chosen, not assumed: the counter does not
// see a small non-escaping class or an array, so a `RouteShape` control read
// as flat for the same reason a broken `pick` would have.
TEST(XpuRouteTable, pickAllocatesNothingOnTheHitPath) {
    int rc = runWith(
        "        R a = heap R(\"a\", Regime.DecodeRow, 10, 12, true);\n"
        "        t.register(a);\n"
        "        Route warm = t.pick(Regime.DecodeRow, 12, s);\n"
        "        if (warm == null) { return 1; }\n"
        "        int64 b0 = Cajeta.allocatedBytes();\n"
        "        int32 k = 0;\n"
        "        while (k < 256) {\n"
        "            Route got = t.pick(Regime.DecodeRow, 12, s);\n"
        "            if (got == null) { return 2; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        int64 b1 = Cajeta.allocatedBytes();\n"
        "        if (b1 != b0) { return 3; }\n"
        "        RouteTable other = heap RouteTable();\n"
        "        if (other.count() != 0) { return 4; }\n"
        "        int64 b2 = Cajeta.allocatedBytes();\n"
        "        if (b2 <= b1) { return 5; }\n"
        "        return 0;\n");
    EXPECT_EQ(rc, 0);
}

