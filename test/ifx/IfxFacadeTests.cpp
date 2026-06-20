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

// ── Unit 7: capability / permission / lifecycle surface (spec §6) ──────────────────────────────

// 7b -- the null floor gates nothing: requesting any permission yields NotRequired (ordinal 0),
// never a throw.
TEST(IfxFacadeTests, nullFloorPermissionIsNotRequired) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.AudioBackend;\n"
        "import cajeta.ifx.NullAudioBackend;\n"
        "import cajeta.ifx.Permission;\n"
        "import cajeta.ifx.PermissionState;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AudioBackend ab = heap NullAudioBackend();\n"
        "        return ab.requestPermission(Permission.Microphone);\n"   // NotRequired == 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 7b -- a denied permission is an OBSERVABLE STATE, not a crash: a backend that denies mic capture
// returns PermissionState.Denied (ordinal 3) from requestPermission without throwing.
TEST(IfxFacadeTests, deniedPermissionIsObservableStateNotThrow) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.AudioBackend;\n"
        "import cajeta.ifx.AudioStream;\n"
        "import cajeta.ifx.Feature;\n"
        "import cajeta.ifx.Permission;\n"
        "import cajeta.ifx.PermissionState;\n"
        "import cajeta.lang.String;\n"
        "public final class DenyingAudio implements AudioBackend {\n"
        "    public DenyingAudio() { }\n"
        "    public boolean probe()    { return true; }\n"
        "    public int32   priority() { return 5; }\n"
        "    public String  name()     { return \"denier\"; }\n"
        "    public boolean supports(Feature feature) { return false; }\n"
        "    public PermissionState requestPermission(Permission permission) {\n"
        "        if (permission == Permission.Microphone) { return PermissionState.Denied; }\n"
        "        return PermissionState.NotRequired;\n"
        "    }\n"
        "    public PermissionState permissionState(Permission permission) {\n"
        "        if (permission == Permission.Microphone) { return PermissionState.Denied; }\n"
        "        return PermissionState.NotRequired;\n"
        "    }\n"
        "    public AudioStream openOutput(uint32 sampleRate, uint32 channels) { return null; }\n"
        "    public void submit(AudioStream s, float32[] frames) { }\n"
        "    public void close(AudioStream s) { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AudioBackend ab = heap DenyingAudio();\n"
        "        PermissionState s = ab.requestPermission(Permission.Microphone);\n"  // no throw
        "        return s;\n"   // PermissionState.Denied == 3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}

// 7c -- the null floor never loses a surface, so pollLifecycle yields no events (null array).
TEST(IfxFacadeTests, nullFloorDeliversNoLifecycleEvents) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowBackend;\n"
        "import cajeta.ifx.NullWindowBackend;\n"
        "import cajeta.ifx.LifecyclePhase;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowBackend wb = heap NullWindowBackend();\n"
        "        LifecyclePhase[] evs = wb.pollLifecycle(null);\n"
        "        if (evs == null) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 7c -- surface-lost / surface-recreated are deliverable as LifecyclePhase events: a backend drains
// {SurfaceLost, SurfaceRecreated} through pollLifecycle and the caller reads them back in order.
TEST(IfxFacadeTests, lifecycleSurfaceEventsAreDeliverable) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowBackend;\n"
        "import cajeta.ifx.Window;\n"
        "import cajeta.ifx.Surface;\n"
        "import cajeta.ifx.WindowEvent;\n"
        "import cajeta.ifx.Feature;\n"
        "import cajeta.ifx.Permission;\n"
        "import cajeta.ifx.PermissionState;\n"
        "import cajeta.ifx.LifecyclePhase;\n"
        "import cajeta.lang.String;\n"
        "public final class EventfulWindow implements WindowBackend {\n"
        "    public EventfulWindow() { }\n"
        "    public boolean probe()    { return true; }\n"
        "    public int32   priority() { return 5; }\n"
        "    public String  name()     { return \"eventful\"; }\n"
        "    public boolean supports(Feature feature) { return false; }\n"
        "    public PermissionState requestPermission(Permission permission) { return PermissionState.NotRequired; }\n"
        "    public PermissionState permissionState(Permission permission)   { return PermissionState.NotRequired; }\n"
        "    public Window createWindow(String title, uint32 width, uint32 height) { return null; }\n"
        "    public Surface surfaceOf(Window w) { return null; }\n"
        "    public #WindowEvent[] poll(Window w) { return null; }\n"
        "    public void destroy(Window w) { }\n"
        "    public #LifecyclePhase[] pollLifecycle(Window w) {\n"
        "        #LifecyclePhase[] evs = {LifecyclePhase.SurfaceLost, LifecyclePhase.SurfaceRecreated};\n"
        "        return evs;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowBackend wb = heap EventfulWindow();\n"
        "        LifecyclePhase[] evs = wb.pollLifecycle(null);\n"
        "        if (evs.count() == 2\n"
        "            && evs[0] == LifecyclePhase.SurfaceLost\n"
        "            && evs[1] == LifecyclePhase.SurfaceRecreated) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 7b -- IfxInfo.supportsWindow is the app-facing facade over the shared registry: with no OS backend
// linked (only the auto-registered null floor), every optional feature reads unsupported.
TEST(IfxFacadeTests, ifxInfoSupportsFacadeRoutesToRegistry) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.IfxInfo;\n"
        "import cajeta.ifx.Feature;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        if (IfxInfo.supportsWindow(Feature.MultiWindow)) { return 1; }\n"   // floor → false
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// ── Unit 8: recording SPI seam (stub-level, spec §7) ──────────────────────────────────────────

// 8a -- the VideoSink seam + its royalty-free PNG-sequence fallback are referenceable from stdlib:
// frames pushed through the seam are recorded WHILE OPEN; writes before open() / after close() drop.
// (Real PNG byte-encoding lives in the external harness; this proves the contract.)
TEST(IfxFacadeTests, videoSinkFallbackCountsFramesWhileOpen) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.PngSequenceVideoSink;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        PngSequenceVideoSink png = heap PngSequenceVideoSink();\n"
        "        png.writeFrame(null);\n"                  // not open → dropped
        "        png.open(1280, 720, 30);\n"
        "        png.writeFrame(null);\n"                  // counted
        "        png.writeFrame(null);\n"                  // counted
        "        png.close();\n"
        "        png.writeFrame(null);\n"                  // closed → dropped
        "        return png.frameCount();\n"               // 2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 2);
}

// 8a -- the fallback is usable THROUGH the VideoSink seam interface (open returns true, name()
// dispatches): this is the shape the external harness implements + registers against.
TEST(IfxFacadeTests, videoSinkReferenceableThroughSeamInterface) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.VideoSink;\n"
        "import cajeta.ifx.PngSequenceVideoSink;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        VideoSink sink = heap PngSequenceVideoSink();\n"
        "        if (sink.open(640, 480, 24) && sink.name().equals(\"png-sequence\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 8a -- the AudioSink seam + its royalty-free WAV fallback: sample-buffers count while open.
TEST(IfxFacadeTests, audioSinkFallbackCountsBuffersWhileOpen) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WavAudioSink;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WavAudioSink wav = heap WavAudioSink();\n"
        "        wav.writeSamples(null);\n"               // not open → dropped
        "        wav.open(48000, 2);\n"
        "        wav.writeSamples(null);\n"               // counted
        "        wav.writeSamples(null);\n"               // counted
        "        wav.writeSamples(null);\n"               // counted
        "        wav.close();\n"
        "        return wav.bufferCount();\n"             // 3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 3);
}

// 8a -- the WAV fallback is usable through the AudioSink seam interface (the harness's shape).
TEST(IfxFacadeTests, audioSinkReferenceableThroughSeamInterface) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.AudioSink;\n"
        "import cajeta.ifx.WavAudioSink;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AudioSink sink = heap WavAudioSink();\n"
        "        if (sink.open(44100, 1) && sink.name().equals(\"wav\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}
