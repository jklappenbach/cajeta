//
// An exact enum argument outranks one that decays to an integer (enum-overload-
// ranking spec §2.1). An enum constant converts to int32, its ordinal, and overload
// resolution used that conversion when nothing matched exactly. The closest-match
// scan then tied at zero for every positional candidate, so
// `heap KernelBuffer<int32>(n, MemoryKind.Pinned)` with a uint32 `n` bound the raw
// (int64 handle, uint64 count) constructor and wrapped address `n`.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t run(const std::string& src, const char* fn = "run") {
    auto jit = CajetaJit::compile(src, "test.D");
    if (!jit) return -999;
    auto f = jit->lookup<int32_t (*)()>(fn);
    return f ? f() : -998;
}

// `decls` are D's overloads; `body` is run()'s.
std::string source(const std::string& decls, const std::string& body) {
    return "package test;\n"
           "public enum Kind {\n"
           "    A,\n"
           "    B,\n"
           "    C;\n"
           "}\n"
           "public final class Box {\n"
           "    public int32 which;\n"
           "    public int64 got;\n"
           "    public Box(uint64 n, Kind k) { this.which = 1; int32 o = k; this.got = (int64) n * 10L + (int64) o; }\n"
           "    public Box(int64 h, uint64 n) { this.which = 2; this.got = h * 1000L + (int64) n; }\n"
           "}\n"
           "public final class Flip {\n"
           "    public int32 which;\n"
           "    public Flip(int64 h, uint64 n) { this.which = 2; }\n"
           "    public Flip(uint64 n, Kind k) { this.which = 1; }\n"
           "}\n"
           "public final class D {\n" + decls +
           "    public static int32 run() {\n" + body +
           "    }\n"
           "}\n";
}

const char* kPair =
    "    static int64 f(uint64 n, Kind k) { int32 o = k; return 1000L + (int64) n * 10L + (int64) o; }\n"
    "    static int64 f(int64 h, uint64 n) { return 2000L + h * 10L + (int64) n; }\n";
const char* kPairFlipped =
    "    static int64 f(int64 h, uint64 n) { return 2000L + h * 10L + (int64) n; }\n"
    "    static int64 f(uint64 n, Kind k) { int32 o = k; return 1000L + (int64) n * 10L + (int64) o; }\n";

} // namespace

// Use case 1: f(n, Kind.B) binds f(uint64, Kind), with n's value and B's ordinal.
TEST(EnumOverloadRanking, anExactEnumOutranksDecayForAMethod) {
    EXPECT_EQ(run(source(kPair, "uint32 n = 7; return (int32) D.f(n, Kind.B);\n")), 1000 + 70 + 1);
    EXPECT_EQ(run(source(kPairFlipped, "uint32 n = 7; return (int32) D.f(n, Kind.B);\n")),
              1000 + 70 + 1);
}

// Use case 2: with no enum argument the integer overload still binds.
TEST(EnumOverloadRanking, aCallWithNoEnumKeepsTheIntegerOverload) {
    EXPECT_EQ(run(source(kPair, "uint32 n = 7; return (int32) D.f(0, n);\n")), 2000 + 7);
    EXPECT_EQ(run(source(kPairFlipped, "uint32 n = 7; return (int32) D.f(0, n);\n")), 2000 + 7);
}

// Use case 3: an enum still reaches an int32-only overload through the decay.
TEST(EnumOverloadRanking, anEnumStillDecaysWhenNoOverloadTakesIt) {
    EXPECT_EQ(run(source("    static int32 g(int32 k) { return 50 + k; }\n",
                         "return D.g(Kind.C);\n")),
              52);
}

// The constructors, which is the KernelBuffer case, in both declaration orders.
TEST(EnumOverloadRanking, anExactEnumOutranksDecayForAConstructor) {
    EXPECT_EQ(run(source("", "uint32 n = 7; Box b = heap Box(n, Kind.B); return (int32) b.got * 10 + b.which;\n")),
              71 * 10 + 1);
    EXPECT_EQ(run(source("", "uint32 n = 7; Flip b = heap Flip(n, Kind.B); return b.which;\n")), 1);
    EXPECT_EQ(run(source("", "uint32 n = 7; Box b = heap Box(0, n); return (int32) b.got * 10 + b.which;\n")),
              7 * 10 + 2);
    EXPECT_EQ(run(source("", "uint32 n = 7; Flip b = heap Flip(0, n); return b.which;\n")), 2);
}
