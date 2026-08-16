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
#include "../PortableEnv.h"   // setenv/unsetenv for the CAJETA_IFX_* override tests

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
        "import cajeta.ifx.IfxInfo;\n"
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
        "import cajeta.ifx.Permission;\n"
        "import cajeta.ifx.PermissionState;\n"
        "import cajeta.ifx.LifecyclePhase;\n"
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
        // advertises exactly one optional feature (MultiWindow) so a test can prove supports()
        // delegation distinguishes a provided capability from an unsupported one.
        "    public boolean supports(Feature feature) { return feature == Feature.MultiWindow; }\n"
        "    public PermissionState requestPermission(Permission permission) { return PermissionState.NotRequired; }\n"
        "    public PermissionState permissionState(Permission permission)   { return PermissionState.NotRequired; }\n"
        "    public Window createWindow(String title, uint32 width, uint32 height) { return null; }\n"
        "    public Surface surfaceOf(Window w) { return null; }\n"
        "    public #WindowEvent[] poll(Window w) { return null; }\n"
        "    public void destroy(Window w) { }\n"
        "    public #LifecyclePhase[] pollLifecycle(Window w) { return null; }\n"
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
        "    public PermissionState requestPermission(Permission permission) { return PermissionState.NotRequired; }\n"
        "    public PermissionState permissionState(Permission permission)   { return PermissionState.NotRequired; }\n"
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
        "    public PermissionState requestPermission(Permission permission) { return PermissionState.NotRequired; }\n"
        "    public PermissionState permissionState(Permission permission)   { return PermissionState.NotRequired; }\n"
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

// 4a (tie-break) -- equal priority resolves to the FIRST-registered backend, deterministically.

// 4b -- a backend that probe()s false is never bound, even at a far higher priority. Headless so the
// fall-through to the floor is silent (not the loud-error path).

// 4d -- only the null floor present + an opt-in headless request -> bind null silently (no throw).

// 4e -- only the null floor present + an interactive window request -> loud IfxException (testable),
// never a silent black screen.

// 4f -- the three domains are selected independently: a real window backend binds while input falls
// to its null floor, each resolved from its own registry.

// Input selection prefers the higher-priority viable backend over the floor (per-domain symmetry).

// Audio has no loud-error case -- falling to the silent floor is a valid headless outcome (no throw).

// ---- Unit 5: bootstrap / auto-registration of the null floor (spec section 5 "always registered") ----
// The process-wide registry, BackendRegistry.instance(), auto-registers the Null* floor on first
// access. A program that links no OS backend still binds -- with NO explicit registerWindow call by
// the app. This is what makes "import ifx and it just works headless" true.

// 5a (window) -- the shared registry already carries the null window floor on first access: an opt-in
// headless request binds it silently, the app having registered nothing.

// 5a (input + audio) -- the input and audio floors are auto-registered too, each domain resolving to
// its own -1000 floor with no app registration.

// 7a -- supports(Feature) wired through the registry: supportsWindow() delegates to the bound
// backend, distinguishing a provided capability (FakeWindow advertises MultiWindow) from an
// unsupported one (Touch), independent of the -1000 null floor also being registered.

// 7a (floor) -- with only the null floor bound in each domain, every optional feature is unsupported.

// 7a (unregistered domain) -- supports*() is robust when a domain has NO backend registered: it
// reports false instead of crashing. (Regression: the bound-backend lookup must not null-compare an
// interface value — comparing an actually-null interface against null miscompiles; supports*()
// tracks presence with a boolean instead.)
TEST(IfxRegistryTests, supportsFalseForUnregisteredDomain) {
    EXPECT_EQ(runI32(
        "BackendRegistry r = heap BackendRegistry();\n"
        "r.registerWindow(heap NullWindowBackend());\n"   // window floor only; input + audio EMPTY
        "int32 acc = 0;\n"
        "if (r.supportsInput(Feature.GamepadRumble)) { acc = acc + 1; }\n"   // empty input  → false
        "if (r.supportsAudio(Feature.Hdr))           { acc = acc + 1; }\n"   // empty audio  → false
        "if (r.supportsWindow(Feature.MultiWindow))  { acc = acc + 1; }\n"   // floor        → false
        "return acc;\n"), 0);
}

// 4e -- IfxInfo.describe() snapshots the bound backend per domain from the shared registry: a real
// window backend registered through instance() shows up as the bound window name (over the floor).

// 4c -- CAJETA_IFX_WINDOW=<name> forces that registered backend over probe()/priority(): the named
// low-priority backend binds even though a higher-priority one is viable.

// 4g -- an unknown CAJETA_IFX_WINDOW value is a loud launch error (IfxException), even for a headless
// request: the operator named a backend that is not registered, so we fail rather than silently ignore.

// 4c (input) -- CAJETA_IFX_INPUT forces the named input backend over priority.

// 4c (audio) -- CAJETA_IFX_AUDIO forces the named audio backend over priority.

// 5b -- instance() is a true process-wide singleton AND is the load-time register() entry external
// backends call: a backend registered through instance() lands in the same registry the app selects
// from, so it wins over the auto-registered floor (also proving select(false) does not fail loudly
// once a real backend is present).
