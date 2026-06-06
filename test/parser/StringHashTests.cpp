// String.hash() — content-based hash overriding Object's identity hash.
//
// Two distinct String instances with identical UTF-8 bytes hash to the
// same value, so Object.operator== (which compares hashes) gives value
// equality for free — String does NOT override operator==, only hash().
//
// Empty / null-bytes / distinct-content cases pinned here. Algorithm
// in v1 is FNV-1a (pure Cajeta); the runtime already has XXH3-64
// (`__cajeta_hash_bytes`) but exposing it to Cajeta requires
// int8[]→uint8_t* bridging that hasn't landed yet. FNV-1a is a stable,
// content-deterministic hash adequate for value equality — the DoS
// defense layer (seed-mix) is a follow-up.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// hash() is deterministic — two calls on the same instance return the
// same value.
TEST(StringHashTests, hashIsDeterministicSameInstance) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[3];\n"
        "        b[0] = (int8) 'a';\n"
        "        b[1] = (int8) 'b';\n"
        "        b[2] = (int8) 'c';\n"
        "        String s = heap String(#b, 3);\n"
        "        int64 h1 = s.hash();\n"
        "        int64 h2 = s.hash();\n"
        "        if (h1 == h2) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Two distinct String instances with identical bytes hash equal —
// the core content-hash semantic.
TEST(StringHashTests, sameContentSameHash) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b1 = heap int8[3];\n"
        "        b1[0] = (int8) 'a';\n"
        "        b1[1] = (int8) 'b';\n"
        "        b1[2] = (int8) 'c';\n"
        "        int8[] b2 = heap int8[3];\n"
        "        b2[0] = (int8) 'a';\n"
        "        b2[1] = (int8) 'b';\n"
        "        b2[2] = (int8) 'c';\n"
        "        String s1 = heap String(#b1, 3);\n"
        "        String s2 = heap String(#b2, 3);\n"
        "        if (s1.hash() == s2.hash()) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Distinct content ⇒ distinct hashes (under FNV-1a; collisions
// theoretically possible but vanishingly rare for 3-byte inputs).
TEST(StringHashTests, distinctContentDistinctHash) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b1 = heap int8[3];\n"
        "        b1[0] = (int8) 'a';\n"
        "        b1[1] = (int8) 'b';\n"
        "        b1[2] = (int8) 'c';\n"
        "        int8[] b2 = heap int8[3];\n"
        "        b2[0] = (int8) 'a';\n"
        "        b2[1] = (int8) 'b';\n"
        "        b2[2] = (int8) 'd';\n"   // last byte differs
        "        String s1 = heap String(#b1, 3);\n"
        "        String s2 = heap String(#b2, 3);\n"
        "        if (s1.hash() == s2.hash()) return 0;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Empty String (null bytes / 0 length) hashes deterministically — same
// value as the FNV-1a offset basis.
TEST(StringHashTests, emptyStringHashesDeterministic) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        String s1 = heap String();\n"  // default ctor: bytes=null, len=0
        "        String s2 = heap String();\n"
        "        if (s1.hash() == s2.hash()) return s1.hash();\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    // FNV-1a offset basis 0xCBF29CE484222325, signed-interpreted.
    EXPECT_EQ(runI64(src), (int64_t) 0xCBF29CE484222325ULL);
}

// End-to-end: two distinct Strings with identical content compare ==
// via Object.operator==. This is the load-bearing reason String needs
// a content hash: without it, == would be pointer identity (distinct
// heap pointers) and value-equality would be impossible without
// overriding operator==.
TEST(StringHashTests, sameContentComparesEqual) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b1 = heap int8[3];\n"
        "        b1[0] = (int8) 'a';\n"
        "        b1[1] = (int8) 'b';\n"
        "        b1[2] = (int8) 'c';\n"
        "        int8[] b2 = heap int8[3];\n"
        "        b2[0] = (int8) 'a';\n"
        "        b2[1] = (int8) 'b';\n"
        "        b2[2] = (int8) 'c';\n"
        "        String s1 = heap String(#b1, 3);\n"
        "        String s2 = heap String(#b2, 3);\n"
        "        if (s1 == s2) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// And distinct content compares !=.
TEST(StringHashTests, distinctContentComparesNotEqual) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b1 = heap int8[3];\n"
        "        b1[0] = (int8) 'a';\n"
        "        b1[1] = (int8) 'b';\n"
        "        b1[2] = (int8) 'c';\n"
        "        int8[] b2 = heap int8[3];\n"
        "        b2[0] = (int8) 'a';\n"
        "        b2[1] = (int8) 'b';\n"
        "        b2[2] = (int8) 'd';\n"
        "        String s1 = heap String(#b1, 3);\n"
        "        String s2 = heap String(#b2, 3);\n"
        "        if (s1 != s2) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
