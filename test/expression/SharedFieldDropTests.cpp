// Unit 3 of the slices plan (drop/move side): stakes held by class fields and
// containers release exactly once through the existing drop chains, and
// #-moves are rc-neutral (slice-spec §6.2). The synthesized value-copy hook
// (spec §6.1) lands with its consumer, the Utf8 value type (Unit 6).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.collection.ArrayList;\n"
           "public final class Holder {\n"
           "    public String s;\n"
           "    public Holder(#String s) { this.s = #s; }\n"
           "}\n"
           "public final class Sfd {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Sfd");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kDyn =
    "String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "String b = \"0123456789\";\n"
    "String s = a + b;\n";

}  // namespace

// 3.1.1/3.1.3 — a slice #-moved into a class field: the object's drop chain
// releases the stake; the root frees exactly once (live count returns to
// baseline across the whole scope — no leak, no double free).
TEST(SharedFieldDropTests, fieldHeldStakeReleasesOnObjectDrop) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "{\n" +
        std::string(kDyn) +
        "    String w = s.substring(5, 30);\n"
        "    Holder h = heap Holder(#w);\n"
        "    if (h.s.size() != 25) { return -1; }\n"
        "    if (h.s.charAt(0) != (int8) 102) { return -2; }\n"   // 'f'
        "}\n"
        "int64 after = Cajeta.liveCount();\n"
        "if (after != base) { return -3; }\n"
        "return 1;"), 1);
}

// 3.1.3 — slices held in a container read correctly and the scope exits
// cleanly (no double free, no dangle), and the liveCount balance holds.
// The balance assert was BLOCKED on the container element-drop gap (slices
// 9.2.1); element-ownership Unit 3 closed it — an OWNING instantiation
// (`ArrayList<#String>`) drops its elements at teardown, so `#`-transferred
// slices belong in one. (The borrow instantiation deliberately does not
// drop — see OwnershipLeakProbe.arrayListBorrowElementsUntouched.)
TEST(SharedFieldDropTests, containerHeldStakesRelease) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "{\n" +
        std::string(kDyn) +
        "    ArrayList<#String> parts = heap ArrayList<#String>();\n"
        "    String p0 = s.substring(0, 10);\n"
        "    String p1 = s.substring(10, 20);\n"
        "    String p2 = s.substring(20, 36);\n"
        "    parts.add(#p0);\n"
        "    parts.add(#p1);\n"
        "    parts.add(#p2);\n"
        "    String e1 = parts.get(1);\n"
        "    if (!e1.equals(\"klmnopqrst\")) { return -1; }\n"
        "    String e2 = parts.get(2);\n"
        "    if (!e2.equals(\"uvwxyz0123456789\")) { return -2; }\n"
        "}\n"
        "int64 after = Cajeta.liveCount();\n"
        "if (after != base) { return -3; }\n"
        "return 1;"), 1);
}

// 3.1.2 — #-move is rc-neutral: a stake moved local -> local -> field is
// dropped exactly once; content stays valid through the chain and the
// balance holds.
TEST(SharedFieldDropTests, moveIsRcNeutral) {
    EXPECT_EQ(runJit(
        "int64 base = Cajeta.liveCount();\n"
        "{\n" +
        std::string(kDyn) +
        "    String w = s.substring(3, 23);\n"
        "    String w2 = #w;\n"                       // move: no retain
        "    Holder h = heap Holder(#w2);\n"          // move into field
        "    if (h.s.size() != 20) { return -1; }\n"
        "    if (!h.s.startsWith(\"def\")) { return -2; }\n"
        "}\n"
        "int64 after = Cajeta.liveCount();\n"
        "if (after != base) { return -3; }\n"
        "return 1;"), 1);
}
