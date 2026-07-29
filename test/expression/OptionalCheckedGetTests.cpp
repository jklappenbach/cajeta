// optional-absence plan 2.1 — checked Optional.get().
//
// get() on empty must throw NoOptionalValueException (recoverable — an
// unwrap miss is catchable; Unrecoverable is reserved for panic), not the
// historical untyped `throw 1` (CAJETA_ERROR_NONE_UNWRAP), which no
// catch-by-type clause can see. get() on present is unchanged: primitive, class-typed,
// and borrowed-field wraps (the Cache.get shape) all extract the value.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.error.Throwable;\n"
           "import cajeta.error.NoOptionalValueException;\n"
           "public class Box {\n"
           "    public String tag;\n"
           "    public Box(#String tag) {\n"
           "        this.tag = tag;\n"
           "    }\n"
           "}\n"
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

} // namespace

// 2.1.1 — empty get() throws NoOptionalValueException; catch by concrete
// type and by the Throwable root; the message names Optional.
TEST(OptionalCheckedGetTests, emptyGetThrowsNoOptionalValue) {
    EXPECT_EQ(runJit(
        "Optional<int32> none = stack Optional<int32>(false, 0);\n"
        "try {\n"
        "    int32 v = none.get();\n"
        "    return -1;\n"                                   // must not reach
        "} catch (NoOptionalValueException e) {\n"
        "    if (e.getMessage().indexOf(\"Optional\") < 0) { return -2; }\n"
        "    return 1;\n"
        "}\n"
        "return -3;"), 1);
}

TEST(OptionalCheckedGetTests, emptyGetCatchableAsThrowable) {
    EXPECT_EQ(runJit(
        "Optional<int32> none = stack Optional<int32>(false, 0);\n"
        "try {\n"
        "    int32 v = none.get();\n"
        "    return -1;\n"
        "} catch (Throwable t) {\n"
        "    return 1;\n"
        "}\n"
        "return -3;"), 1);
}

// 2.1.2 — present get() unchanged: primitive, and a class-typed local
// moved in with `#` (locals require transfer; only FIELDS wrap plain,
// the Cache.get shape, covered by CacheTests upstream).
TEST(OptionalCheckedGetTests, presentGetExtracts) {
    EXPECT_EQ(runJit(
        "Optional<int32> some = stack Optional<int32>(true, 42);\n"
        "if (some.get() != 42) { return -1; }\n"
        "Box b = heap Box(#\"tagged\");\n"
        "Optional<Box> obox = stack Optional<Box>(true, #b);\n"  // local moves in
        "Box back = obox.get();\n"
        "if (!(back.tag == \"tagged\")) { return -2; }\n"
        "Optional<int32> none = stack Optional<int32>(false, 0);\n"
        "if (none.orElse(-7) != -7) { return -3; }\n"            // orElse never throws
        "return 1;"), 1);
}
