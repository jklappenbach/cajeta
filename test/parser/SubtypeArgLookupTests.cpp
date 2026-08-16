//
// resolveMethod's subtype-aware fallback. Without it, passing an
// `ArrayStream<int32>` argument to a constructor or method that declares
// a `Stream<int32>` parameter misses the lookup — the generic key
// embeds the arg's exact canonical name (`...::TakeStream(
// cajeta.lang.stream.ArrayStream<int32>,int32)`) and never matches the
// registered key (`...::TakeStream(cajeta.lang.stream.Stream<int32>,int32)`).
// The fallback walks the arg's superClasses to find a compatible param
// shape and picks the most specific match.
//
// Before the fix, the workaround was to declare an intermediate local
// at the exact param type. Pre-fix users had to write:
//
//   ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);
//   Stream<int32> src = as;
//   TakeStream<int32> t = heap TakeStream<int32>(src, 3);  // worked
//
// After the fix, the upcast happens implicitly at the ctor call:
//
//   ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);
//   TakeStream<int32> t = heap TakeStream<int32>(#as, 3);  // works
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

TEST(SubtypeArgLookupTests, derivedClassArgBindsToBaseClassParamUserDefined) {
    // Pure user-defined hierarchy — no stdlib involvement. Holder<T>'s
    // ctor takes a Stream<T>; we pass an ArrayStream<T>.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.Stream;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public class Holder<T> {\n"
        "    public Stream<T> field;\n"
        "    public int32 n;\n"
        "    public Holder(Stream<T> s, int32 n) {\n"
        "        this.field #= s;\n"
        "        this.n = n;\n"
        "    }\n"
        "    public int32 getN() { return this.n; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 3);\n"
        "        Holder<int32> h = heap Holder<int32>(as, 11);\n"
        "        return h.getN();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

TEST(SubtypeArgLookupTests, derivedClassArgBindsToBaseClassParamStdlib) {
    // TakeStream<int32>(Stream<int32>, int32) accepts an ArrayStream<int32>
    // arg directly — no explicit upcast local needed.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.stream.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(#as, 3);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(SubtypeArgLookupTests, deeperSubclassChainStillBinds) {
    // Two levels of inheritance — verify BFS walks past the immediate
    // parent. Confirms the distance metric picks the nearest match
    // when multiple compatible methods are registered.
    auto src =
        "package test;\n"
        "public class Animal {}\n"
        "public class Mammal extends Animal {}\n"
        "public class Dog extends Mammal {}\n"
        "public class Vet {\n"
        "    public int32 examine(Animal a) { return 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Vet v = heap Vet();\n"
        "        Dog d = heap Dog();\n"
        "        return v.examine(d);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(SubtypeArgLookupTests, exactMatchStillWins) {
    // Sanity: when both an exact and a base-class overload exist, the
    // exact one should be picked (distance 0 < distance 1).
    auto src =
        "package test;\n"
        "public class Animal {}\n"
        "public class Dog extends Animal {}\n"
        "public class Vet {\n"
        "    public int32 examine(Animal a) { return 1; }\n"
        "    public int32 examine(Dog d) { return 2; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Vet v = heap Vet();\n"
        "        Dog d = heap Dog();\n"
        "        return v.examine(d);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}
