// Tests for cajeta.collection.ArrayList<T> — minimal P6.7 surface.
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

TEST(ArrayListTests, emptyListSizeIsZero) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        return list.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(ArrayListTests, emptyListIsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        if (list.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(ArrayListTests, addAndSizeIncrement) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(10);\n"
        "        list.add(20);\n"
        "        list.add(30);\n"
        "        return list.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(ArrayListTests, addAndGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(10);\n"
        "        list.add(20);\n"
        "        list.add(30);\n"
        "        return list.get(0) + list.get(1) + list.get(2);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

TEST(ArrayListTests, setReplacesElement) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(1);\n"
        "        list.add(2);\n"
        "        list.set(0, 99);\n"
        "        return list.get(0) + list.get(1);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 101);
}

TEST(ArrayListTests, streamWalksAddedElements) {
    // ArrayList.stream() returns a heap-allocated ArrayStream<T>
    // walking the live elements. count() walks via next() to
    // exhaustion.
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(5);\n"
        "        list.add(10);\n"
        "        list.add(15);\n"
        "        ArrayStream<int32> s = list.stream();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(ArrayListTests, growsBeyondInitialCapacity) {
    // Default capacity is 16 — push 20 elements to exercise the grow
    // path.
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 20) {\n"
        "            list.add(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return list.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}
