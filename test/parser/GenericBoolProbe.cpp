//
// Probe: boolean field on a generic class instantiation.
// Optional<T> in P6.2 trips LLVM ICmp type mismatch on `if (opt.isPresent())`.
// Isolating the smallest reproducer here.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(GenericBoolProbe, nonGenericBooleanNegationWorks) {
    // Probe: non-generic class with boolean field + unary `!` in a
    // method body. Optional<T>'s isEmpty() uses this shape and trips
    // an LLVM ICmp type mismatch in the stdlib path. This probe
    // determines whether `!` itself is broken or only in combination
    // with generic instantiation.
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    boolean flag;\n"
        "    public Holder(boolean f) { this.flag = f; }\n"
        "    public boolean isFlagged() { return this.flag; }\n"
        "    public boolean isClear() { return !this.flag; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder(false);\n"
        "        if (h.isClear()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(GenericBoolProbe, nonGenericBooleanFieldWorks) {
    // Baseline: non-generic class with a boolean field + getter.
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    boolean flag;\n"
        "    public Holder(boolean f) { this.flag = f; }\n"
        "    public boolean isFlagged() { return this.flag; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder(true);\n"
        "        if (h.isFlagged()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(GenericBoolProbe, genericBooleanFieldFails) {
    // Same shape, but the class is generic. Reading the boolean field
    // through the generic instantiation may trip LLVM ICmp mismatch.
    auto src =
        "package test;\n"
        "public class Holder<T> {\n"
        "    boolean flag;\n"
        "    T value;\n"
        "    public Holder(boolean f, T v) { this.flag = f; this.value = v; }\n"
        "    public boolean isFlagged() { return this.flag; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder<int32> h = heap Holder<int32>(true, 42);\n"
        "        if (h.isFlagged()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
