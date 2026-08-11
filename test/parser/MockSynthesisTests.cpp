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
