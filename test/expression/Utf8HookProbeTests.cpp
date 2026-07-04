// Unit 6b — direct probes of the Utf8 rc intrinsics (Cajeta.utf8Retain /
// utf8Release / sharedPopulation): explicit retain/release round-trip and
// the poison-after-release double-free guard. These pin the internal ABI
// the synthesized copy/drop hooks lower onto.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Ut {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// Probe A: explicit release via the intrinsic — isolates the native from the
// drop-entry wiring.
TEST(Utf8HookProbeTests, explicitReleaseRestores) {
    EXPECT_EQ(runJit(
        "int64 pop = Cajeta.sharedPopulation();\n"
        "String a = \"abcdefghijklm\";\n"
        "String b = \"nopqrstuvwxyz\";\n"
        "String s = a + b;\n"
        "Utf8 v = Utf8.of(s);\n"
        "if (Cajeta.sharedPopulation() != pop + 1) { return -1; }\n"
        "Cajeta.utf8Release(v);\n"
        "if (Cajeta.sharedPopulation() != pop) { return -2; }\n"
        "return 1;"), 1);
}

// Probe B: explicit retain then double release — checks retain and the
// poison-after-release guard.
TEST(Utf8HookProbeTests, explicitRetainRelease) {
    EXPECT_EQ(runJit(
        "int64 pop = Cajeta.sharedPopulation();\n"
        "String a = \"abcdefghijklm\";\n"
        "String b = \"nopqrstuvwxyz\";\n"
        "String s = a + b;\n"
        "Utf8 v = Utf8.of(s);\n"
        "Cajeta.utf8Retain(v);\n"
        "Cajeta.utf8Release(v);\n"
        "if (Cajeta.sharedPopulation() != pop + 1) { return -1; }\n"
        "Cajeta.utf8Release(v);\n"
        "if (Cajeta.sharedPopulation() != pop + 1) { return -2; }\n"  // poisoned: no-op
        "return 1;"), 1);
}
