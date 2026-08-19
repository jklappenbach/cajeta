//
// stdlib-ownership-convention U4 — JsonValue.setString ownership (4.1.1-4.1.3).
//
// The spec's centerpiece (§2.5/§2.6): `setString(String)` today branches on
// `s.root() == null || s.byteOffset() != 0` — a runtime property of the
// argument the caller cannot see. A plain-root String is stored as a BORROW,
// so a JsonValue that outlives its source string reads back recycled memory.
// After the 4.2.1 migration setString COPIES unconditionally and the aliasing
// variant moves to `setStringBorrowed`.
//
// Detection is DATA SURVIVAL, the OwnershipArrayCanaryTests method: build the
// value in a helper so the source drops at helper exit, churn allocations to
// recycle the freed block, then read back through the JsonValue. liveCount
// balance cannot distinguish a correct copy from a lend that never
// transferred; content can.
//
// RED as written (2026-08-18): the root-path borrow dangles in
// setStringCopiesWhenSourceDropsFirst / temporarySourceCorruptionRepro, and
// setStringBorrowedAliasesWhileSourceLives does not compile until 4.2.1 adds
// the method. All three go green with the migration, no edits here.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -1;
}

// Shared scaffold: `make()` builds a JsonValue whose source String is built
// at runtime (heap-backed root, offset 0 — the exact branch that borrows
// today) and DROPS at make() exit. `run()` churns and reads back.
const char* kSourceDropsFirst =
    "package test;\n"
    "import cajeta.codec.json.JsonValue;\n"
    "public final class D {\n"
    "    static #JsonValue make() {\n"
    // 32 bytes — past the 24-byte SSO region, so the String keeps a heap
    // ROOT and setString's `s.root() == null` guard does NOT reroute to
    // the copy path. A short payload silently tests the wrong branch.
    "        int8[] kb #= heap int8[32];\n"
    "        int32 i = 0;\n"
    "        while (i < 32) {\n"
    "            kb[i] = (int8) (97 + (i % 4));\n"  // abcdabcd...
    "            i = i + 1;\n"
    "        }\n"
    "        String s #= heap String(#kb, 32);\n"
    "        JsonValue v #= heap JsonValue();\n"
    "        v.setString(s);\n"
    "        return #= v;\n"          // s drops here; v must not care
    "    }\n"
    "    public static int32 run() {\n"
    "        JsonValue v #= D.make();\n"
    "        int32 j = 0;\n"          // recycle the freed block so a dangle
    "        while (j < 8) {\n"       // reads DIFFERENT bytes, not stale ones
    "            int8[] c = heap int8[32];\n"
    "            int32 k = 0;\n"
    "            while (k < 32) { c[k] = (int8) 90; k = k + 1; }\n"
    "            j = j + 1;\n"
    "        }\n"
    "        String out #= v.asString();\n"
    "        if (out == null) { return -2; }\n"
    "        if (out.byteLength() != 32) { return -3; }\n"
    "        if (out.equals(\"abcdabcdabcdabcdabcdabcdabcdabcd\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

}  // namespace

// 4.1.1 — the exact Unit 13 failure as a stdlib test: the JsonValue outlives
// the source String and still reads back correctly. Requires setString to
// copy (4.2.1); the current root-path borrow dangles.
TEST(JsonValueOwnershipTests, setStringCopiesWhenSourceDropsFirst) {
    EXPECT_EQ(1, runI32(kSourceDropsFirst));
}

// 4.1.3 — the cajeta-llama corruption repro: the source is a TEMPORARY
// (String concat), dead before the read. Same detector, hotter source.
TEST(JsonValueOwnershipTests, temporarySourceCorruptionRepro) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    static #JsonValue make(int32 i) {\n"
        "        JsonValue v #= heap JsonValue();\n"
        // 31-char literal + digit = 32 bytes: past SSO, so the temp carries
        // a heap root and the borrow branch is the one under test.
        "        v.setString(\"0123456789012345678901234567890\" + i);\n"
        "        return #= v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        JsonValue v #= D.make(7);\n"
        "        int32 j = 0;\n"
        "        while (j < 8) {\n"
        "            int8[] c = heap int8[32];\n"
        "            int32 k = 0;\n"
        "            while (k < 32) { c[k] = (int8) 90; k = k + 1; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        String out #= v.asString();\n"
        "        if (out == null) { return -2; }\n"
        "        if (out.equals(\"01234567890123456789012345678907\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

// 4.1.2 — the aliasing variant is separately named and its lifetime bound is
// the documented one: valid while the source lives. (The dangle case is UB by
// contract, not a testable behavior — what this pins is that the SHARP
// spelling exists, is explicit, and works within its bound.) Does not compile
// until 4.2.1 introduces setStringBorrowed.
TEST(JsonValueOwnershipTests, setStringBorrowedAliasesWhileSourceLives) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.json.JsonValue;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] kb #= heap int8[3];\n"
        "        kb[0] = (int8) 120;\n"  // 'x'
        "        kb[1] = (int8) 121;\n"  // 'y'
        "        kb[2] = (int8) 122;\n"  // 'z'
        "        String s #= heap String(#kb, 3);\n"
        "        JsonValue v #= heap JsonValue();\n"
        "        v.setStringBorrowed(s);\n"
        "        String out #= v.asString();\n"  // source still live here
        "        if (out == null) { return -2; }\n"
        "        if (out.equals(\"xyz\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}
