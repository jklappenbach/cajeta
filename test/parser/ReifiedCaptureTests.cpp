//
// ReifiedCaptureTests — reified-capture-plan.md: recovering a concrete
// instantiation from a template wildcard. Because cajeta monomorphizes, a value
// widened to `Box<?>` is still its concrete `Box<int32>` at runtime, so
// `instanceof Box<int32>` is a sound reified RTTI check and `(Box<int32>) w`
// recovers the typed handle (representation-identical — same pointer).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* BOX =
    "package test;\n"
    "public class Box<T> {\n"
    "    T value;\n"
    "    public Box(T v) { this.value = v; }\n"
    "    public T get() { return this.value; }\n"
    "}\n";

} // namespace

// instanceof over a wildcard: true for the real instantiation, false for another.
TEST(ReifiedCaptureTests, instanceofMatchesReifiedInstantiation) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Box<int32>) { r = r + 1; }\n"
        "        if (w instanceof Box<float32>) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// pattern binding: `w instanceof Box<int32> f` binds `f : Box<int32>` in the
// matched branch; reading through it sees the concrete object.
TEST(ReifiedCaptureTests, instanceofBindingCapture) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(99);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<int32> f) {\n"
        "            return f.get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// pattern binding on a mismatched dtype does not enter the branch.
TEST(ReifiedCaptureTests, instanceofBindingMismatchSkips) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<float32> f) {\n"
        "            return 1;\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -1);
}

// tryAs on a MATCH: returns a present Optional holding the same object.
TEST(ReifiedCaptureTests, tryAsPresentOnMatch) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        Optional<Box<int32>> o = w.tryAs<Box<int32>>();\n"
        "        if (o.isPresent()) { return o.get().get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// tryAs on a MISMATCH: returns an empty Optional (no throw, no mis-typed ptr).
TEST(ReifiedCaptureTests, tryAsEmptyOnMismatch) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        Optional<Box<float32>> o = w.tryAs<Box<float32>>();\n"
        "        if (o.isEmpty()) { return 99; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// capture: a guarded cast recovers the concrete handle and reads through it.
TEST(ReifiedCaptureTests, capturedCastReadsConcrete) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<int32>) {\n"
        "            Box<int32> cap = (Box<int32>) w;\n"
        "            return cap.get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// unguarded capture cast on a MATCH: no instanceof guard, the reified check
// passes, the concrete handle is recovered and read.
TEST(ReifiedCaptureTests, unguardedCastSucceedsOnMatch) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        Box<int32> cap = (Box<int32>) w;\n"
        "        return cap.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// unguarded capture cast on a MISMATCH: throws cajeta.error.ClassCastException
// (catchable) rather than handing back a mis-typed pointer (UB).
TEST(ReifiedCaptureTests, unguardedCastThrowsClassCastException) {
    std::string src =
        "package test;\n"
        "import cajeta.error.ClassCastException;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            Box<float32> bad = (Box<float32>) w;\n"
        "            result = 100;\n"
        "        } catch (ClassCastException e) {\n"
        "            result = 7;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
