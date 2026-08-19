//
// stdlib-ownership-convention U10.1 — the nested-class type-resolution
// family, written down (found 2026-08-03 building the build-tool plugins;
// specs/INDEX.md row "nested-class-type-resolution"; carried since only in
// a memory note). Three signatures, one family:
//
//   1. `Optional<Outer.Nested>` resolves RAW — the type argument is
//      dropped, `isPresent()` is "no member on cajeta.lang.Optional".
//      Sidestep (NOT the fix): hoist Nested to top level.
//   2. WRONG-OUTER bind — an unqualified nested name inside its own outer
//      binds to a same-named nested class of ANOTHER outer (the nested
//      variant of classpath-signature-shortname-rebind, whose top-level
//      half was fixed in 8d8d87ff). Sidestep: unique nested names.
//   3. Chained `.get()` off an Optional-of-nested resolves the member
//      against the nested class, not Optional. Sidestep: two-step local.
//
// These tests assert the CORRECT behavior — RED while a defect stands,
// green with the fix, unedited. MEASURED 2026-08-19:
//   * unqualifiedNestedBindsToOwnOuter PASSES — the wrong-outer bind was
//     fixed by 8d8d87ff (shortname-rebind's placeholder-fill fix covered
//     the nested variant). It stays ENABLED as the regression pin.
//   * The Optional-of-nested pair still FAILS — those two are DISABLED_
//     so a documented defect does not poison the gate; enable with the
//     fix (they should go green with no edit).
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

}  // namespace

// 10.1.1 — Optional<Outer.Nested> must keep its type argument: isPresent()
// and get() resolve, and the held value reads back.
TEST(NestedClassResolutionTests, DISABLED_optionalOfNestedKeepsTypeArgument) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Outer {\n"
        "    public class Nested {\n"
        "        public int32 v;\n"
        "        public Nested(int32 v) { this.v = v; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<Outer.Nested> o =\n"
        "            heap Optional<Outer.Nested>(heap Outer.Nested(41));\n"
        "        if (!o.isPresent()) { return -2; }\n"
        "        Outer.Nested n = o.get();\n"
        "        return n.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(41, runI32(src));
}

// 10.1.2 — an unqualified nested name used inside its own outer class binds
// to THAT outer's nested class, not a same-named nested of another outer.
// OuterB (with the decoy) is declared FIRST so a first-registered-wins
// rebind is the one this catches.
TEST(NestedClassResolutionTests, unqualifiedNestedBindsToOwnOuter) {
    std::string src =
        "package test;\n"
        "public class OuterB {\n"
        "    public class Entry {\n"
        "        public int32 tag;\n"
        "        public Entry() { this.tag = 200; }\n"
        "    }\n"
        "}\n"
        "public class OuterA {\n"
        "    public class Entry {\n"
        "        public int32 tag;\n"
        "        public Entry() { this.tag = 100; }\n"
        "    }\n"
        "    public int32 makeOne() {\n"
        "        Entry e = heap Entry();\n"   // unqualified — must be OuterA.Entry
        "        return e.tag;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        OuterA a = heap OuterA();\n"
        "        return a.makeOne();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(100, runI32(src));
}

// 10.1.3 — a chained `.get()` off an Optional-of-nested resolves against
// Optional (yielding the nested value), not against the nested class.
TEST(NestedClassResolutionTests, DISABLED_chainedGetResolvesAgainstOptional) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Outer {\n"
        "    public class Nested {\n"
        "        public int32 v;\n"
        "        public Nested(int32 v) { this.v = v; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    static #Optional<Outer.Nested> find() {\n"
        "        Optional<Outer.Nested> o =\n"
        "            heap Optional<Outer.Nested>(heap Outer.Nested(7));\n"
        "        return #= o;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return D.find().get().v;\n"   // chained — no two-step local
        "    }\n"
        "}\n";
    EXPECT_EQ(7, runI32(src));
}
