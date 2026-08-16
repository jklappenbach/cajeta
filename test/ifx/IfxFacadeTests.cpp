//
// IfxFacadeTests — cajeta.ifx window-domain contract (STACK item: "Surface + Window +
// WindowEvent contract"). The facade is platform-agnostic, so these run on any host (incl. this
// Windows box). They pin: (1) cajeta.ifx parses on demand via the IfxInfo anchor, and (2) the
// window-domain value types construct and expose their fields.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::Compiler;

// cajeta.ifx resolves through the stdlib loader: importing IfxInfo makes the anchor method run
// end-to-end, and the package reports parsed.
TEST(IfxFacadeTests, ifxPackageParsesViaIfxInfo) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.IfxInfo;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return IfxInfo.version();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);   // IfxInfo.version() == 1
    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.ifx"));
}

// Unit 6 (stdlib build integration) — cajeta.ifx is an EAGER stdlib package (only cajeta.math is
// lazy), so the whole package — contract value types + the Backend SPI + the Null* floor +
// BackendRegistry + IfxException — is prescanned and parsed at Compiler startup, before and without
// any ifx import. A user program that never mentions ifx still has cajeta.ifx parsed: proof the SPI
// / floor / registry eager-compile cleanly as part of the embedded stdlib build (no eager crash).
TEST(IfxFacadeTests, ifxPackageIsEagerlyParsedWithoutImport) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"   // deliberately no cajeta.ifx import
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
    // Eager: parsed at startup with nothing in the user source referencing it.
    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.ifx"));
}

// Surface is an opaque value type — it constructs and reports its drawable extent.
TEST(IfxFacadeTests, surfaceConstructsAndReportsExtent) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.Surface;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Surface s = heap Surface(0, 5, 1280, 720);\n"   // nativeHandle 0 => headless
        "        return (int32) s.surfaceWidth();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1280);
}

// WindowEvent is the portable event value type — it constructs and reports its kind.
TEST(IfxFacadeTests, windowEventConstructsAndReportsType) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowEvent;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowEvent e = heap WindowEvent(6, 0, 0.0f, 0.0f, 0, 0);\n"   // 6 == CLOSE
        "        return e.eventType();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 6);
}

// Window is the contract handle type — it constructs and returns its opaque handle.
TEST(IfxFacadeTests, windowConstructsAndReturnsHandle) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.Window;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Window w = heap Window(42);\n"
        "        return (int32) w.handle();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// The capability/lifecycle/permission vocabulary — simple int32-aliased enums whose ordinals are
// the stable contract a backend reports against. (The supports(Feature) query that consumes them
// wires up with the backend registry — next stack item.)



// Input + audio domain contract value types (siblings of the window-domain types).


// Backend SPI: the three null floors implement their per-domain interfaces (which extend Backend),
// reachable through interface dispatch — each reports the floor priority -1000 (sum -3000).

// The null floor probes viable but supports no optional Feature (via the Backend base methods).

// ── Unit 7: capability / permission / lifecycle surface (spec §6) ──────────────────────────────

// 7b -- the null floor gates nothing: requesting any permission yields NotRequired (ordinal 0),
// never a throw.

// 7b -- a denied permission is an OBSERVABLE STATE, not a crash: a backend that denies mic capture
// returns PermissionState.Denied (ordinal 3) from requestPermission without throwing.

// 7c -- the null floor never loses a surface, so pollLifecycle yields no events (null array).

// 7c -- surface-lost / surface-recreated are deliverable as LifecyclePhase events: a backend drains
// {SurfaceLost, SurfaceRecreated} through pollLifecycle and the caller reads them back in order.

// 7b -- IfxInfo.supportsWindow is the app-facing facade over the shared registry: with no OS backend
// linked (only the auto-registered null floor), every optional feature reads unsupported.

// 4e -- IfxInfo.describe() snapshots the active per-domain bind. With only the auto-registered floor
// (no OS backend linked), every domain reports the "null" floor — and describe() is headless-tolerant
// (it must never raise the interactive loud-error just to report what's bound).
TEST(IfxFacadeTests, ifxInfoDescribeSnapshotsFloorBind) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.IfxInfo;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IfxInfo info #= IfxInfo.describe();\n"
        "        if (info.windowBackendName().equals(\"null\")\n"
        "            && info.inputBackendName().equals(\"null\")\n"
        "            && info.audioBackendName().equals(\"null\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// ── Unit 8: recording SPI seam (stub-level, spec §7) ──────────────────────────────────────────

// 8a -- the VideoSink seam + its royalty-free PNG-sequence fallback are referenceable from stdlib:
// frames pushed through the seam are recorded WHILE OPEN; writes before open() / after close() drop.
// (Real PNG byte-encoding lives in the external harness; this proves the contract.)

// 8a -- the fallback is usable THROUGH the VideoSink seam interface (open returns true, name()
// dispatches): this is the shape the external harness implements + registers against.

// 8a -- the AudioSink seam + its royalty-free WAV fallback: sample-buffers count while open.

// 8a -- the WAV fallback is usable through the AudioSink seam interface (the harness's shape).
