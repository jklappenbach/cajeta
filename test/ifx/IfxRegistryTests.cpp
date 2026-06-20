//
// IfxRegistryTests -- cajeta.ifx Unit 4: BackendRegistry + dispatch (the keystone).
// Platform-agnostic (pure-Cajeta registry, no OS code), so these run on any host. They pin the
// registry's selection contract (spec section 4 / cajeta-gfx-spec section 9.4):
//   4a  highest priority() among viable backends binds;
//   4b  a backend whose probe()==false is never bound, even at higher priority;
//   4d  with only the null floor, an opt-in headless request binds null silently;
//   4e  with only the null floor, an interactive window request fails loudly (IfxException);
//   4f  window / input / audio are selected independently.
// The CAJETA_IFX_* env override (4c/4g) is deferred -- no pure-Cajeta env primitive yet.
//
// Landing these exercised (and drove the fix of) nine compiler codegen bugs around interface
// values: closure-arg pass-by-pointer ABI, interface array element store/read, two interface
// dispatch-receiver paths, interface-by-value return, interface superclass linking by load order,
// sret-for-reference-class returns, and a signed/unsigned relational-comparison defect. See the
// memory note refarray-drop-codegen-bug for the full trail.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Declares configurable fakes for all three domains plus the entry class D, then splices `body`
// into D.run() (which returns int32). A FakeXxx reports a caller-chosen name/priority/viability so
// a test can prove which backend the registry bound and why.
std::string withFakes(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.ifx.BackendRegistry;\n"
        "import cajeta.ifx.WindowBackend;\n"
        "import cajeta.ifx.InputBackend;\n"
        "import cajeta.ifx.AudioBackend;\n"
        "import cajeta.ifx.NullWindowBackend;\n"
        "import cajeta.ifx.NullInputBackend;\n"
        "import cajeta.ifx.NullAudioBackend;\n"
        "import cajeta.ifx.Window;\n"
        "import cajeta.ifx.Surface;\n"
        "import cajeta.ifx.WindowEvent;\n"
        "import cajeta.ifx.InputDevice;\n"
        "import cajeta.ifx.AudioStream;\n"
        "import cajeta.ifx.Feature;\n"
        "import cajeta.ifx.IfxException;\n"
        "import cajeta.lang.String;\n"
        // a fake window backend: caller picks name / priority / viability
        "public final class FakeWindow implements WindowBackend {\n"
        "    private String id;\n"
        "    private int32 prio;\n"
        "    private boolean viable;\n"
        "    public FakeWindow(String id, int32 prio, boolean viable) {\n"
        "        this.id = id; this.prio = prio; this.viable = viable;\n"
        "    }\n"
        "    public boolean probe()    { return this.viable; }\n"
        "    public int32   priority() { return this.prio; }\n"
        "    public String  name()     { return this.id; }\n"
        "    public boolean supports(Feature feature) { return false; }\n"
        "    public Window createWindow(String title, uint32 width, uint32 height) { return null; }\n"
        "    public Surface surfaceOf(Window w) { return null; }\n"
        "    public WindowEvent[] poll(Window w) { return null; }\n"
        "    public void destroy(Window w) { }\n"
        "}\n"
        // a fake input backend
        "public final class FakeInput implements InputBackend {\n"
        "    private String id;\n"
        "    private int32 prio;\n"
        "    private boolean viable;\n"
        "    public FakeInput(String id, int32 prio, boolean viable) {\n"
        "        this.id = id; this.prio = prio; this.viable = viable;\n"
        "    }\n"
        "    public boolean probe()    { return this.viable; }\n"
        "    public int32   priority() { return this.prio; }\n"
        "    public String  name()     { return this.id; }\n"
        "    public boolean supports(Feature feature) { return false; }\n"
        "    public int32 gamepadCount() { return 0; }\n"
        "    public InputDevice gamepad(int32 index) { return null; }\n"
        "    public boolean buttonDown(InputDevice d, int32 button) { return false; }\n"
        "    public float32 axis(InputDevice d, int32 axis) { return 0.0f; }\n"
        "}\n"
        // a fake audio backend
        "public final class FakeAudio implements AudioBackend {\n"
        "    private String id;\n"
        "    private int32 prio;\n"
        "    private boolean viable;\n"
        "    public FakeAudio(String id, int32 prio, boolean viable) {\n"
        "        this.id = id; this.prio = prio; this.viable = viable;\n"
        "    }\n"
        "    public boolean probe()    { return this.viable; }\n"
        "    public int32   priority() { return this.prio; }\n"
        "    public String  name()     { return this.id; }\n"
        "    public boolean supports(Feature feature) { return false; }\n"
        "    public AudioStream openOutput(uint32 sampleRate, uint32 channels) { return null; }\n"
        "    public void submit(AudioStream s, float32[] frames) { }\n"
        "    public void close(AudioStream s) { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(withFakes(body), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : INT32_MIN;
}

} // namespace

// 4a -- among viable backends, the highest priority() binds (the real fake beats the -1000 floor).
TEST(IfxRegistryTests, highestPriorityViableBackendBinds) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap NullWindowBackend());\n"        // floor, prio -1000
        "r.registerWindow(heap FakeWindow(\"real\", 10, true));\n"
        "WindowBackend b = r.selectWindow(false);\n"
        "return b.priority();\n"), 10);
}

// 4a (tie-break) -- equal priority resolves to the FIRST-registered backend, deterministically.
TEST(IfxRegistryTests, priorityTieBreaksToFirstRegistered) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap FakeWindow(\"first\", 5, true));\n"
        "r.registerWindow(heap FakeWindow(\"second\", 5, true));\n"
        "WindowBackend b = r.selectWindow(false);\n"
        "if (b.name().equals(\"first\")) { return 1; }\n"
        "return 0;\n"), 1);
}

// 4b -- a backend that probe()s false is never bound, even at a far higher priority. Headless so the
// fall-through to the floor is silent (not the loud-error path).
TEST(IfxRegistryTests, nonViableBackendNeverBoundEvenAtHigherPriority) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap FakeWindow(\"broken\", 9999, false));\n"   // high prio, NOT viable
        "r.registerWindow(heap NullWindowBackend());\n"
        "WindowBackend b = r.selectWindow(true);\n"
        "return b.priority();\n"), -1000);   // the floor bound, not the non-viable high-prio fake
}

// 4d -- only the null floor present + an opt-in headless request -> bind null silently (no throw).
TEST(IfxRegistryTests, headlessRequestBindsNullFloorSilently) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap NullWindowBackend());\n"
        "WindowBackend b = r.selectWindow(true);\n"
        "if (b.name().equals(\"null\")) { return 1; }\n"
        "return 0;\n"), 1);
}

// 4e -- only the null floor present + an interactive window request -> loud IfxException (testable),
// never a silent black screen.
TEST(IfxRegistryTests, interactiveRequestWithOnlyNullFloorFailsLoudly) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap NullWindowBackend());\n"
        "try {\n"
        "    WindowBackend b = r.selectWindow(false);\n"
        "    return 0;\n"                       // reached only if no throw -- a regression
        "} catch (IfxException e) {\n"
        "    return 1;\n"                        // the loud-error path fired
        "}\n"), 1);
}

// 4f -- the three domains are selected independently: a real window backend binds while input falls
// to its null floor, each resolved from its own registry.
TEST(IfxRegistryTests, domainsSelectIndependently) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap FakeWindow(\"real\", 10, true));\n"
        "r.registerWindow(heap NullWindowBackend());\n"
        "r.registerInput(heap NullInputBackend());\n"   // only the floor for input
        "WindowBackend w = r.selectWindow(false);\n"
        "InputBackend  i = r.selectInput();\n"
        "if (w.priority() == 10 && i.priority() == -1000) { return 1; }\n"
        "return -1;\n"), 1);
}

// Input selection prefers the higher-priority viable backend over the floor (per-domain symmetry).
TEST(IfxRegistryTests, inputSelectsHighestPriorityViable) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerInput(heap NullInputBackend());\n"
        "r.registerInput(heap FakeInput(\"pad\", 7, true));\n"
        "InputBackend i = r.selectInput();\n"
        "return i.priority();\n"), 7);
}

// Audio has no loud-error case -- falling to the silent floor is a valid headless outcome (no throw).
TEST(IfxRegistryTests, audioFallsToSilentFloorWithoutThrowing) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerAudio(heap NullAudioBackend());\n"
        "AudioBackend a = r.selectAudio();\n"
        "if (a.name().equals(\"null\")) { return 1; }\n"
        "return 0;\n"), 1);
}
