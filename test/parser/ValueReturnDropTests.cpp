//
// M5(c) — caller-scope drop for value returns. Before the fix, a value-
// returning method was classified as a borrow (non-`#` return) and the
// receiving local registered NO drop entry, so the user destructor never
// fired and any owned fields silently leaked. After the fix, the local takes
// the stack-drop variant (drops fields + user destructor, doesn't free the
// body alloca) — the same path a direct `stack X(...)` initializer uses.
//
// Observability via the Cajeta.dropCount/dropCountReset intrinsics that other
// destructor tests use. Box has no class-typed fields (sidesteps a separate,
// pre-existing limitation in stack-drop's field-ownership semantics where a
// borrowed class field gets dropped as if owned).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Returns the drop count observed after run() exits the inner scope. > 0
// means the stack-drop fn fired for the value-returned local.
int64_t runAndCountDrops(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    return jit->lookup<int64_t (*)()>("read")();
}

} // namespace

TEST(ValueReturnDropTests, userDestructorFiresOnValueReturnAtScopeExit) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    int32 val;\n"
        "    public Box(int32 v) { this.val = v; }\n"
        "    public ~Box() {\n"
        "        int32 unused = this.val;\n"
        "    }\n"
        "}\n"
        "public class Maker {\n"
        "    public Box make() {\n"
        "        return stack Box(42);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Maker m = heap Maker();\n"
        "        {\n"
        "            Box b = m.make();\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_GT(runAndCountDrops(src), 0);
}
