// Object.operator==(Object) — default identity equality via hash().
//
// The implementation is `this.hash() == other.hash()`. For Object's
// default identity hash (pointer-derived, splitmix64-finalized) this
// gives pointer identity: distinct pointers ⇒ distinct hashes ⇒
// returns 0. Same pointer ⇒ same hash ⇒ returns 1.
//
// Subclasses that override hash() to return a value-based hash
// (notably cajeta.lang.String) automatically get value-equality from
// the same operator== shape — virtual dispatch on hash() carries the
// semantic shift.
//
// hash() returns int64; collision probability between two distinct
// pointers under splitmix64 is ~2^-64, so identity comparison via
// hash is exact in practice. (For value-based hashes, collisions
// exist but are vanishingly rare with a cryptographic finalizer.)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Same instance under `==` returns true (1).
TEST(ObjectOperatorEqTests, sameInstanceEqEqIsTrue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        if (d == d) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Two distinct heap instances ⇒ different identity hashes ⇒ not equal.
TEST(ObjectOperatorEqTests, distinctInstancesEqEqIsFalse) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        D a = heap D();\n"
        "        D b = heap D();\n"
        "        if (a == b) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Alias (two references to the same heap instance) ⇒ same hash ⇒
// equal. Pins that the comparison really walks through hash() rather
// than something that diverges per-variable.
TEST(ObjectOperatorEqTests, aliasEqEqIsTrue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        D a = heap D();\n"
        "        D b = a;\n"
        "        if (a == b) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `!=` is the negation of `==`. Distinct instances are NOT equal,
// so `!=` returns 1 (true).
TEST(ObjectOperatorEqTests, distinctInstancesNeIsTrue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        D a = heap D();\n"
        "        D b = heap D();\n"
        "        if (a != b) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Same instance under `!=` returns false. Pins the != / == duality.
TEST(ObjectOperatorEqTests, sameInstanceNeIsFalse) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        if (d != d) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
