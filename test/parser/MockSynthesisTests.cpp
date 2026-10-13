//
// @GenerateMock / SynthesizedMockClass (specs/archive/mock-codegen-spec.md),
// exercised IN-PROCESS for the first time (test-battery-restructure 4.3 —
// the 107 lines of SynthesizedMockClass.cpp were 0% covered: the feature's
// consumers live in cajeta-unit, a sibling library, so nothing here ever
// compiled an @GenerateMock class).
//
// The generated Mock<Name> body forwards every overridable target method
// through `dev.cajeta.unit.MockEngine.handle(name, #args)` — boxing
// primitive args (`Int32.of(k)`), unboxing primitive returns
// (`((Int32) handle(...)).value()`), downcasting reference returns, and
// discarding the answer for void. cajeta-unit itself is NOT on the test
// classpath; the multi-source compile supplies a minimal stand-in
// MockEngine under the same canonical, which is all the synthesized body
// binds against — the mock-side codegen under test is identical.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// Minimal stand-in for the cajeta-unit engine: counts calls, answers by
// method name. Boxed Int32 for the primitive-return scenario, a String for
// the reference-return scenario, a throwaway box for void.
const char* kEngineStandIn =
    "package dev.cajeta.unit;\n"
    "import cajeta.lang.Int32;\n"
    "public class MockEngine {\n"
    "    public int32 calls;\n"
    "    public MockEngine() { this.calls = 0; }\n"
    "    public Object handle(String name, #Object[] a) {\n"
    "        this.calls = this.calls + 1;\n"
    "        if (name.equals(\"fetch\")) { return Int32.of(42); }\n"
    "        if (name.equals(\"label\")) { return \"stubbed\"; }\n"
    "        return Int32.of(0);\n"
    "    }\n"
    "}\n";

// The target: three overridable shapes — primitive-arg/primitive-return
// (box + unbox), reference return (downcast), and void (answer discarded).
const char* kTarget =
    "package test;\n"
    "@GenerateMock\n"
    "public class Gateway {\n"
    "    public Gateway() { }\n"
    "    public int32 fetch(int32 k) { return 7; }\n"
    "    public String label() { return \"real\"; }\n"
    "    public void ping() { }\n"
    "}\n";

const char* kDriver =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        int32 score = 0;\n"
    "        MockGateway m = heap MockGateway();\n"
    // Primitive round trip: the stub answer (42), not the real body (7).
    "        if (m.fetch(5) == 42) { score = score + 1; }\n"
    // Reference return: downcast of the engine's answer.
    "        if (m.label().equals(\"stubbed\")) { score = score + 2; }\n"
    // Void forward: no value, but the engine must have been consulted.
    "        m.ping();\n"
    "        if (m.engine.calls == 3) { score = score + 4; }\n"
    // The mock IS-A target: assignable where the real class is expected.
    "        Gateway g = m;\n"
    "        if (g.fetch(9) == 42) { score = score + 8; }\n"
    "        return score;\n"
    "    }\n"
    "}\n";
} // namespace

// The @GenerateMock use-case matrix, one compile: the compiler synthesizes
// MockGateway beside the annotated target; its overrides forward through
// the engine (primitive box/unbox, reference downcast, void discard), and
// the mock substitutes for the target type.
TEST(MockSynthesisTests, generateMockForwardsStubsAndSubstitutesForTarget) {
    std::map<std::string, std::string> sources = {
        {"dev.cajeta.unit.MockEngine", kEngineStandIn},
        {"test.Gateway", kTarget},
        {"test.D", kDriver},
    };
    auto jit = CajetaJit::compile(sources, "test.D");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 15)
        << "score bits: 1 primitive stub round-trip; 2 reference downcast; "
           "4 engine consulted for all three calls; 8 mock substitutes "
           "for the target type";
}

// mock-codegen M5 (primavera-web 0.2.6.1): interface targets, the inherited
// method walk, final and static filtering, owned returns, and a name taken.
namespace {

// The stand-in engine again, with handleOwned: the owned answer a `#R`
// method needs, which `handle`'s borrowed answer cannot give.
const char* kEngineOwned =
    "package dev.cajeta.unit;\n"
    "import cajeta.lang.Int32;\n"
    "import test.Rec;\n"
    "public class MockEngine {\n"
    "    public int32 calls;\n"
    "    public MockEngine() { this.calls = 0; }\n"
    "    public Object handle(String name, #Object[] a) {\n"
    "        this.calls = this.calls + 1;\n"
    "        if (name.equals(\"count\") || name.equals(\"fetch\") || name.equals(\"width\")) { return Int32.of(42); }\n"
    "        if (name.equals(\"label\")) { return \"stubbed\"; }\n"
    "        return Int32.of(0);\n"
    "    }\n"
    "    public #Object handleOwned(String name, #Object[] a) {\n"
    "        this.calls = this.calls + 1;\n"
    "        if (name.equals(\"find\")) { return heap Rec(9); }\n"
    "        return null;\n"
    "    }\n"
    "}\n";

const char* kRec =
    "package test;\n"
    "public class Rec {\n"
    "    public static int32 gone = 0;\n"
    "    public int32 v;\n"
    "    public Rec(int32 v) { this.v = v; }\n"
    "    public ~Rec() { Rec.gone = Rec.gone + 1; }\n"
    "}\n";

int32_t runMock(const std::map<std::string, std::string>& sources) {
    auto jit = CajetaJit::compile(sources, "test.D");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

} // namespace

// `@GenerateMock` on an interface: `MockDirectory implements Directory`,
// forwarding each method, an owned `#Rec` return through handleOwned.
TEST(MockSynthesisTests, interfaceTargetImplementsAndForwards) {
    const char* target =
        "package test;\n"
        "@GenerateMock\n"
        "public interface Directory {\n"
        "    String label();\n"
        "    int32 count(int32 k);\n"
        "    void ping();\n"
        "    #Rec find(String id);\n"
        "}\n";
    const char* driver =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        MockDirectory m = heap MockDirectory();\n"
        "        Directory d = m;\n"
        "        if (d.label().equals(\"stubbed\")) { score = score + 1; }\n"
        "        if (d.count(3) == 42) { score = score + 2; }\n"
        "        d.ping();\n"
        "        {\n"
        "            Rec r #= d.find(\"x\");\n"
        "            if (r.v == 9) { score = score + 4; }\n"
        "        }\n"
        "        if (Rec.gone == 1) { score = score + 8; }\n"
        "        if (m.engine.calls == 4) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runMock({{"dev.cajeta.unit.MockEngine", kEngineOwned}, {"test.Rec", kRec},
                       {"test.Directory", target}, {"test.D", driver}}), 31)
        << "1 reference, 2 primitive, 4 owned return, 8 freed once by the caller, 16 all four forwarded";
}

// A class target's inherited methods are forwarded too; a final method is not
// overridden, so it keeps its real body.
TEST(MockSynthesisTests, inheritedMethodsForwardAndFinalKeepsItsBody) {
    const char* target =
        "package test;\n"
        "public class Base {\n"
        "    public Base() { }\n"
        "    public int32 fetch(int32 k) { return 7; }\n"
        "}\n"
        "@GenerateMock\n"
        "public class Gateway extends Base {\n"
        "    public Gateway() { }\n"
        "    public String label() { return \"real\"; }\n"
        "    public final int32 width() { return 3; }\n"
        "    public static int32 unit() { return 5; }\n"
        "}\n";
    const char* driver =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        MockGateway m = heap MockGateway();\n"
        "        if (m.fetch(1) == 42) { score = score + 1; }\n"
        "        if (m.label().equals(\"stubbed\")) { score = score + 2; }\n"
        "        if (m.width() == 3) { score = score + 4; }\n"
        "        if (Gateway.unit() == 5) { score = score + 8; }\n"
        "        if (m.engine.calls == 2) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runMock({{"dev.cajeta.unit.MockEngine", kEngineOwned}, {"test.Rec", kRec},
                       {"test.Gateway", target}, {"test.D", driver}}), 31)
        << "1 inherited forwarded, 2 own forwarded, 4 final kept, 8 static untouched, 16 two forwards";
}

// An interface that extends another: the mock implements both levels.
TEST(MockSynthesisTests, interfaceTargetForwardsItsParentInterfaceMethods) {
    const char* target =
        "package test;\n"
        "public interface Narrow { String label(); }\n"
        "@GenerateMock\n"
        "public interface Wide extends Narrow { int32 width(); }\n";
    const char* driver =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        MockWide m = heap MockWide();\n"
        "        Wide w = m;\n"
        "        Narrow n = m;\n"
        "        if (w.width() == 42) { score = score + 1; }\n"
        "        if (n.label().equals(\"stubbed\")) { score = score + 2; }\n"
        "        if (w.label().equals(\"stubbed\")) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runMock({{"dev.cajeta.unit.MockEngine", kEngineOwned}, {"test.Rec", kRec},
                       {"test.Wide", target}, {"test.D", driver}}), 7);
}

// A declared class already named Mock<Name> is reported, not silently refilled.
TEST(MockSynthesisTests, aDeclaredClassNamedLikeTheMockIsAnError) {
    const char* target =
        "package test;\n"
        "@GenerateMock\n"
        "public class Gateway {\n"
        "    public Gateway() { }\n"
        "    public String label() { return \"real\"; }\n"
        "}\n"
        "public class MockGateway {\n"
        "    public MockGateway() { }\n"
        "}\n";
    const char* driver =
        "package test;\n"
        "public final class D { public static int32 run() { return 0; } }\n";
    try {
        CajetaJit::compile(std::map<std::string, std::string>{
            {"dev.cajeta.unit.MockEngine", kEngineOwned}, {"test.Rec", kRec},
            {"test.Gateway", target}, {"test.D", driver}}, "test.D");
        FAIL() << "expected CAJETA_ERROR_MOCK_NAME_TAKEN";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MOCK_NAME_TAKEN") << e.getMessage();
    }
}

