// Tests for HashMap's stream views — keys(), values(), entries().
// Each constructs a HashMap, puts N entries, then exercises the
// stream via Stream<T>.count() (terminal) and Stream<T>.forEach
// (terminal with side effect into a static counter).

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

TEST(HashMapStreamTests, keysCountMatchesMapCount) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(1), 10);\n"
        "        m.put(new Tag(2), 20);\n"
        "        m.put(new Tag(3), 30);\n"
        "        return m.keys().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(HashMapStreamTests, valuesCountMatchesMapCount) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(1), 10);\n"
        "        m.put(new Tag(2), 20);\n"
        "        return m.values().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(HashMapStreamTests, entriesCountMatchesMapCount) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(1), 10);\n"
        "        m.put(new Tag(2), 20);\n"
        "        m.put(new Tag(3), 30);\n"
        "        m.put(new Tag(4), 40);\n"
        "        return m.entries().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

TEST(HashMapStreamTests, valuesForEachSumsAllValues) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public class Acc { public static int32 total = 0; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(1), 10);\n"
        "        m.put(new Tag(2), 20);\n"
        "        m.put(new Tag(3), 30);\n"
        "        m.values().forEach((int32 v) -> Acc.total = Acc.total + v);\n"
        "        return Acc.total;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

TEST(HashMapStreamTests, streamSkipsTombstones) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t1 = new Tag(1);\n"
        "        Tag t2 = new Tag(2);\n"
        "        Tag t3 = new Tag(3);\n"
        "        m.put(t1, 10);\n"
        "        m.put(t2, 20);\n"
        "        m.put(t3, 30);\n"
        "        m.remove(t2);\n"
        "        return m.keys().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(HashMapStreamTests, emptyMapStreamCountIsZero) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        return m.keys().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
