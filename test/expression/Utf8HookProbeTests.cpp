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


// ---- the side table GROWS (2026-08-23) --------------------------------------
//
// The table was a fixed 1<<14 array whose insert silently returned once three
// quarters full, after one warning. A promotion that fails to insert has
// ALREADY set the buffer's shared bit, so its release finds no entry and the
// buffer can never be freed: a sizing limit turned into unbounded memory loss,
// in exactly the programs big enough to reach it.
//
// Not theoretical. `cajeta-coco` parsing a 2.7 MB cross-reference index
// saturated the table during PARSING, before doing any work — measured 333
// promotions for 104 site rows and 11,954+ for 13,684 xref records, both
// linear in retained data. Nothing leaked; the table was too small for a
// program that holds an index in memory.
//
// This drives past the old 12,288 cap with every owner held live, so the
// promotions cannot drain. On the old runtime the population pins at 12,288
// and stderr carries the warning; it must now exceed it.

TEST(Utf8HookProbeTests, sideTableGrowsPastTheOldFixedCap) {
    EXPECT_EQ(runJit(
        "cajeta.collection.ArrayList<cajeta.lang.String> keep #= "
        "heap cajeta.collection.ArrayList<cajeta.lang.String>();\n"
        "int32 i = 0;\n"
        "while (i < 20000) {\n"
        // A distinct heap buffer per iteration; Utf8.of promotes it, and the
        // list keeps the owner alive so the stake cannot drop.
        "    String a = \"abcdefghijklm\";\n"
        "    String s #= a + \"nopqrstuvwxyz\";\n"
        "    Utf8 v = Utf8.of(s);\n"
        "    keep.add(#s);\n"
        "    i = i + 1;\n"
        "}\n"
        "if (Cajeta.sharedPopulation() <= 12288) { return -1; }\n"
        "return 1;"), 1)
        << "the shared side table did not grow past the old fixed load cap; "
           "promotions beyond it mark buffers shared with no entry, and those "
           "buffers can never be freed";
}
