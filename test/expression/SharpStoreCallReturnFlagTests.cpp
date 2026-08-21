//
// `dst #= call()` must read the callee's RUNTIME return flag, not a static
// stance derived from the declared return type.
//
// A plain (non-`#`) return is NOT statically a borrow (CLAUDE.md §2.1): a
// plain-return wrapper that tail-calls a `#` method rides the inner flag
// through. `Json.parse<T>(String)` is exactly that shape — it declares `T`
// and tail-calls the synthesized `Json.parse<T>(bytes, len)`, which hands
// back a title.
//
// Before the fix, MoveExpression baked `bindingTakesTitle()` (a compile-time
// answer) into the assignee's drop entry via __cajeta_drop_set_flag. For a
// plain-`T` declaration that constant is 0, so the entry was pushed armed and
// immediately DISARMED — the result object was never claimed, and every owned
// member reachable only from it leaked with it. Measured law before the fix:
// leak == 1 + (one per owned member), per parse.
//
// Detection is liveCount BALANCE across a warmed cycle. Note the instrument's
// limits (CLAUDE.md §5): a balanced count is also consistent with a transfer
// that never happened, so the shapes below vary the MEMBER COUNT — a fixed
// overhead and a per-member leak are distinguishable only that way.
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

// One warm cycle first (lazy per-T synthesis, interned literals and the
// stdlib's own one-shot allocations all land on the FIRST call and would
// otherwise read as a leak), then measure four more.
std::string cycle(const std::string& decls, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "import cajeta.lang.Cajeta;\n"
        + decls +
        "public final class D {\n"
        "    static void once() {\n" + body + "    }\n"
        "    public static int32 run() {\n"
        "        D.once();\n"
        "        int64 a = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 4) { D.once(); i = i + 1; }\n"
        "        return (int32) (Cajeta.liveCount() - a);\n"
        "    }\n"
        "}\n";
}

}  // namespace

// The plain-return generic, no owned members: the RESULT OBJECT itself is the
// fixed part of the leak. This is the minimal case and it fires without any
// String, array or nested object involved.
TEST(SharpStoreCallReturnFlagTests, typedParseResultObjectIsDropped) {
    EXPECT_EQ(0, runI32(cycle(
        "public class C0 { public int32 a; public int32 b; }\n",
        "        C0 c #= Json.parse<C0>(\"{\\\"a\\\":1,\\\"b\\\":2}\");\n")));
}

// Owned members ride the result object down. One String leaked 2/parse before
// the fix (object + String), which is why the earlier reading of this bug as
// "the members are stored as borrows" was wrong — see the member-count ladder
// in the two tests below.
TEST(SharpStoreCallReturnFlagTests, typedParseOwnedStringMemberIsDropped) {
    EXPECT_EQ(0, runI32(cycle(
        "public class C1 { public String s1; public int32 b; }\n",
        "        C1 c #= Json.parse<C1>(\"{\\\"s1\\\":\\\"x\\\",\\\"b\\\":2}\");\n")));
}

// Three Strings leaked 4/parse before the fix. The ladder (1, 2, 3 members ->
// 2, 3, 4 leaked) is what identified the constant term as the result object
// rather than a per-member binding defect.
TEST(SharpStoreCallReturnFlagTests, typedParseScalesWithOwnedMemberCount) {
    EXPECT_EQ(0, runI32(cycle(
        "public class C3 { public String s1; public String s2;\n"
        "                  public String s3; public int32 b; }\n",
        "        C3 c #= Json.parse<C3>(\"{\\\"s1\\\":\\\"x\\\","
        "\\\"s2\\\":\\\"y\\\",\\\"s3\\\":\\\"z\\\",\\\"b\\\":2}\");\n")));
}

// A nested DTO is reachable only through the result object, so it leaked with
// it. Transitive, not one level deep.
TEST(SharpStoreCallReturnFlagTests, typedParseNestedObjectIsDropped) {
    EXPECT_EQ(0, runI32(cycle(
        "public class Inner { public int32 z; }\n"
        "public class Outer { public Inner inner; public int32 b; }\n",
        "        Outer c #= Json.parse<Outer>"
        "(\"{\\\"inner\\\":{\\\"z\\\":1},\\\"b\\\":2}\");\n")));
}

// The other half of the check (CLAUDE.md §5): arming a drop must not
// double-free the ABSENT case. A member the document does not supply is left
// at its default and must still be safe to walk.
TEST(SharpStoreCallReturnFlagTests, typedParseAbsentOwnedMemberIsSafeToDrop) {
    EXPECT_EQ(0, runI32(cycle(
        "public class Inner { public int32 z; }\n"
        "public class Outer { public Inner inner; public int32 b; }\n",
        "        Outer c #= Json.parse<Outer>(\"{\\\"b\\\":2}\");\n")));
}

// The general shape, with no JSON in sight: `#=` from a call is now decided by
// the callee's flag. A `#`-returning callee was already correct before the fix
// (its static stance and its runtime flag agree); this pins that the switch to
// the runtime flag did not regress it into a missed drop or a double free.
TEST(SharpStoreCallReturnFlagTests, sharpStoreFromOwnedReturningCallStillBalances) {
    EXPECT_EQ(0, runI32(
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public class Cfg { public int32 a; public int32 b; }\n"
        "public final class D {\n"
        "    public static #Cfg fresh() { return heap Cfg(); }\n"
        "    static void once() { Cfg c #= D.fresh(); }\n"
        "    public static int32 run() {\n"
        "        D.once();\n"
        "        int64 a = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 4) { D.once(); i = i + 1; }\n"
        "        return (int32) (Cajeta.liveCount() - a);\n"
        "    }\n"
        "}\n"));
}
