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
TEST(IfxFacadeTests, featureEnumOrdinalsAreStable) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.Feature;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Feature.Touch;\n"   // 5
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 5);
}

TEST(IfxFacadeTests, permissionStateEnumOrdinals) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.PermissionState;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return PermissionState.Granted;\n"   // 2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 2);
}

TEST(IfxFacadeTests, lifecyclePhaseEnumOrdinals) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.LifecyclePhase;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return LifecyclePhase.SurfaceRecreated;\n"   // 3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}

// Input + audio domain contract value types (siblings of the window-domain types).
TEST(IfxFacadeTests, inputDeviceConstructsAndReportsIndex) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.InputDevice;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        InputDevice d = heap InputDevice(3);\n"
        "        return d.deviceIndex();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}

TEST(IfxFacadeTests, audioStreamConstructsAndReportsRate) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.AudioStream;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AudioStream s = heap AudioStream(0, 48000, 2);\n"
        "        return (int32) s.streamSampleRate();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 48000);
}

// Backend SPI: the three null floors implement their per-domain interfaces (which extend Backend),
// reachable through interface dispatch — each reports the floor priority -1000 (sum -3000).
TEST(IfxFacadeTests, nullFloorsImplementAllThreeBackendInterfaces) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowBackend;\n"
        "import cajeta.ifx.AudioBackend;\n"
        "import cajeta.ifx.InputBackend;\n"
        "import cajeta.ifx.NullWindowBackend;\n"
        "import cajeta.ifx.NullAudioBackend;\n"
        "import cajeta.ifx.NullInputBackend;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowBackend wb = heap NullWindowBackend();\n"
        "        AudioBackend  ab = heap NullAudioBackend();\n"
        "        InputBackend  ib = heap NullInputBackend();\n"
        "        return wb.priority() + ab.priority() + ib.priority();\n"   // -3000
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), -3000);
}

// The null floor probes viable but supports no optional Feature (via the Backend base methods).
TEST(IfxFacadeTests, nullFloorProbesViableButSupportsNothing) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowBackend;\n"
        "import cajeta.ifx.NullWindowBackend;\n"
        "import cajeta.ifx.Feature;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowBackend wb = heap NullWindowBackend();\n"
        "        int32 p = 0;\n"
        "        if (wb.probe()) { p = p + 1; }\n"                      // +1
        "        if (wb.supports(Feature.Touch)) { p = p + 100; }\n"   // +0 (floor supports nothing)
        "        return p;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}
