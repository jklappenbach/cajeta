//
// NumericBoundsTests — numeric-bounds-plan.md: the Numeric/Floating/Integral
// markers that PRIMITIVES satisfy intrinsically (the type-flag lattice), usable
// as template-parameter bounds (`<T extends Floating>`) and wildcard bounds
// (`Tensor<? extends Floating>`). Floats admitted, int/bool rejected — at compile
// time, zero runtime cost. Trait-style conformance, not Java boxing.
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

// True if the source fails to compile (rejected) — for bound-admission negatives.
bool rejected(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        if (!jit) return true;
        auto fn = jit->lookup<int32_t (*)()>("run");
        return fn == nullptr;
    } catch (...) {
        return true;
    }
}

} // namespace

// `<T extends Floating>` parameter bound admits a float primitive and runs.
TEST(NumericBoundsTests, boundedParamFloatingAdmitsFloat) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static T pick<T extends Floating>(T a, T b) { return a; }\n"
        "    public static int32 run() {\n"
        "        float32 r = D.pick<float32>(2.5f, 1.0f);\n"
        "        if (r != 2.5f) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `<T extends Floating>` rejects an integer type argument.
TEST(NumericBoundsTests, boundedParamFloatingRejectsInt) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static T pick<T extends Floating>(T a, T b) { return a; }\n"
        "    public static int32 run() {\n"
        "        int32 r = D.pick<int32>(2, 1);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(rejected(src));
}

// The sound dtype-generic mechanism: a routine `<T extends Floating>(Tensor<T> t)`
// admits any float tensor (monomorphized per concrete dtype — no variance hazard).
TEST(NumericBoundsTests, dtypeGenericTensorRoutineAdmitsFloat) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class D {\n"
        "    public static int64 elems<T extends Floating>(Tensor<T> t) { return t.size(); }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> t = Tensor.arange<float32>(4);\n"
        "        if (D.elems<float32>(t) != 4) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The same routine rejects an integer tensor (int32 ⊭ Floating) at compile time.
TEST(NumericBoundsTests, dtypeGenericTensorRoutineRejectsInt) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class D {\n"
        "    public static int64 elems<T extends Floating>(Tensor<T> t) { return t.size(); }\n"
        "    public static int32 run() {\n"
        "        Tensor<int32> t = Tensor.arange<int32>(4);\n"
        "        int64 n = D.elems<int32>(t);\n"
        "        return (int32) n;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(rejected(src));
}

// Class-template bound: `class C<T extends Numeric>` admits a numeric primitive.
TEST(NumericBoundsTests, classBoundNumericAdmits) {
    std::string src =
        "package test;\n"
        "public class Cell<T extends Numeric> {\n"
        "    T v;\n"
        "    public Cell(T v) { this.v = v; }\n"
        "    public T get() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell<int32> c = heap Cell<int32>(5);\n"
        "        return c.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Nominal conformance: a user class that `implements Numeric` satisfies the
// `<T extends Numeric>` bound (the predicate's nominal branch — resolve
// cajeta.lang.Numeric, isParentOrKind), alongside the intrinsic primitive path.
TEST(NumericBoundsTests, classBoundNumericAdmitsNominalImpl) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Numeric;\n"
        "public class MyNum implements Numeric {\n"
        "    int32 v;\n"
        "    public MyNum(int32 v) { this.v = v; }\n"
        "    public int32 val() { return this.v; }\n"
        "}\n"
        "public class Cell<T extends Numeric> {\n"
        "    T item;\n"
        "    public Cell(T item) { this.item = item; }\n"
        "    public T get() { return this.item; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MyNum n = heap MyNum(13);\n"
        "        Cell<MyNum> c = heap Cell<MyNum>(#n);\n"
        "        return c.get().val();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// ...and a class that does NOT implement Numeric is rejected for the bound.
TEST(NumericBoundsTests, classBoundNumericRejectsNonImpl) {
    std::string src =
        "package test;\n"
        "public class Plain {\n"
        "    int32 v;\n"
        "    public Plain(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Cell<T extends Numeric> {\n"
        "    T item;\n"
        "    public Cell(T item) { this.item = item; }\n"
        "    public T get() { return this.item; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain p = heap Plain(13);\n"
        "        Cell<Plain> c = heap Cell<Plain>(#p);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(rejected(src));
}

// ...and rejects bool (boolean is not Numeric, despite carrying integer flags).
TEST(NumericBoundsTests, classBoundNumericRejectsBool) {
    std::string src =
        "package test;\n"
        "public class Cell<T extends Numeric> {\n"
        "    T v;\n"
        "    public Cell(T v) { this.v = v; }\n"
        "    public T get() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell<boolean> c = heap Cell<boolean>(true);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(rejected(src));
}

// --- item 4: bare `Tensor<? extends Floating>` variable form (numeric 5a/5b) --

// A mutable primitive container modeled on Tensor->Storage: Holder<T> writes its
// own T into an internal Storage<T> via `this.store.set(v)`. Forming the bounded
// wildcard `Holder<? extends Floating>` must NOT eagerly codegen Holder's mutator
// bodies (which would trip PECS on the own-T internal write and have no concrete
// primitive width). The bounded-wildcard value is an abstract handle; operate by
// capturing to concrete.
namespace {
const char* CONTAINER =
    "package test;\n"
    "import cajeta.lang.Floating;\n"
    "import cajeta.lang.Numeric;\n"
    "public class Storage<T extends Numeric> {\n"
    "    T slot;\n"
    "    public Storage(T v) { this.slot = v; }\n"
    "    public T get() { return this.slot; }\n"
    "    public void set(T v) { this.slot = v; }\n"
    "}\n"
    "public class Holder<T extends Numeric> {\n"
    "    Storage<T> store;\n"
    "    public Holder(Storage<T> s) { this.store = s; }\n"
    "    public T read() { return this.store.get(); }\n"
    "    public void write(T v) { this.store.set(v); }\n"
    "}\n";
} // namespace

// The bare bounded-wildcard variable form compiles and runs — forming and
// assigning `Holder<? extends Floating>` no longer trips the internal-write PECS
// wall (the mutator body isn't eagerly codegen'd for the abstract handle).
TEST(NumericBoundsTests, bareWildcardVarFormsAndAssigns) {
    std::string src = std::string(CONTAINER) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Storage<float32> s = heap Storage<float32>(1.5f);\n"
        "        Holder<float32> h = heap Holder<float32>(s);\n"
        "        Holder<? extends Floating> w = h;\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Operate on a bounded-wildcard value by CAPTURING back to the concrete type
// (reified-capture), then calling methods on the concrete handle (numeric 5b).
TEST(NumericBoundsTests, bareWildcardOperateViaCapture) {
    std::string src = std::string(CONTAINER) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Storage<float32> s = heap Storage<float32>(2.5f);\n"
        "        Holder<float32> h = heap Holder<float32>(s);\n"
        "        Holder<? extends Floating> w = h;\n"
        "        Holder<float32> cap = (Holder<float32>) w;\n"
        "        float32 r = cap.read();\n"
        "        if (r == 2.5f) { return 1; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Acceptance on the REAL cajeta.math.Tensor (numeric 5a/5b/6): a float tensor
// widens to the bare bounded-wildcard `Tensor<? extends Floating>`, then captures
// back to the concrete `Tensor<float32>` to operate (size() is a producer/read).
TEST(NumericBoundsTests, bareWildcardRealTensorOperateViaCapture) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Floating;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 4;\n"
        "        Tensor<float32> z = Tensor.zeros<float32>(shp);\n"
        "        Tensor<? extends Floating> w = z;\n"
        "        Tensor<float32> cap = (Tensor<float32>) w;\n"
        "        return (int32) cap.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// Suppressing the wildcard body must NOT weaken PECS for genuine EXTERNAL writes:
// calling a mutator directly through the `? extends` handle is still rejected
// (the value isn't sourced from the same receiver — unsound covariant write).
TEST(NumericBoundsTests, bareWildcardExternalWriteStillRejected) {
    std::string src = std::string(CONTAINER) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Storage<float32> s = heap Storage<float32>(1.0f);\n"
        "        Holder<float32> h = heap Holder<float32>(s);\n"
        "        Holder<? extends Floating> w = h;\n"
        "        w.write(3.0f);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(rejected(src));
}
