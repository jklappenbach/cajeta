// String defines `<`, `>`, `<=` and `>=` by compareTo (field-store-ownership 6.2.8): bytewise
// UTF-8 order, never the order of the Strings' addresses, and null orders before every String.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "import cajeta.collection.BPlusTree;\n"
           "import cajeta.collection.RedBlackTree;\n"
           "import cajeta.collection.Sort;\n"
           "public final class G {\n"
           "    public static boolean lt<T>(T a, T b) { return a < b; }\n"
           "    public static boolean gt<T>(T a, T b) { return a > b; }\n"
           "}\n"
           "public final class D {\n"
           "    public static #String key(int32 v) {\n"
           "        int8[] buf = Cajeta.allocBytes(12L);\n"
           "        int32 i = 11;\n"
           "        int32 x = v;\n"
           "        while (i >= 0) { buf[i] = (int8) (48 + (x - (x / 10) * 10)); x = x / 10; i = i - 1; }\n"
           "        return heap String(#buf, 12);\n"
           "    }\n"
           "    public static int32 scramble(int32 i, int32 n) { return (i * 37 + 11) - ((i * 37 + 11) / n) * n; }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = D.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// Allocated in reverse order, so an address comparison would answer every case backwards.
TEST(StringOrderingTests, operatorsFollowByteOrderNotAddresses) {
    EXPECT_EQ(runJit(
        "String z #= \"zebra\" + 1;\n"
        "String a #= \"apple\" + 1;\n"
        "String a2 #= \"apple\" + 1;\n"
        "if (!(a < z) || a > z || z < a || !(z > a)) { return -1; }\n"
        "if (!(a <= a2) || !(a >= a2) || a < a2 || a > a2) { return -2; }\n"
        "if (!(a <= z) || a >= z) { return -3; }\n"
        "String ab #= \"ab\" + \"\";\n"
        "String abc #= \"abc\" + \"\";\n"
        "if (!(ab < abc) || abc < ab) { return -4; }\n"
        "return 1;"), 1);
}

TEST(StringOrderingTests, nullOrdersFirst) {
    EXPECT_EQ(runJit(
        "String a #= \"a\" + 1;\n"
        "String n = null;\n"
        "if (!(n < a) || a < n || n > a || !(a > n)) { return -1; }\n"
        "if (n < n || !(n <= n) || !(n >= n)) { return -2; }\n"
        "return 1;"), 1);
}

TEST(StringOrderingTests, genericCodeUsesTheOperators) {
    EXPECT_EQ(runJit(
        "String z #= \"zebra\" + 1;\n"
        "String a #= \"apple\" + 1;\n"
        "if (!G.lt<String>(a, z) || G.lt<String>(z, a)) { return -1; }\n"
        "if (!G.gt<String>(z, a) || G.gt<String>(a, z)) { return -2; }\n"
        "return 1;"), 1);
}

TEST(StringOrderingTests, naturalSortOfOwnedStrings) {
    EXPECT_EQ(runJit(
        "int32 n = 100;\n"
        "String[] a = heap String[n];\n"
        "int32 i = 0;\n"
        "while (i < n) { a[i] #= D.key(D.scramble(i, n)); i = i + 1; }\n"
        "Sort.sort<String>(a, n);\n"
        "i = 0;\n"
        "while (i < n) { if (!a[i].equals(D.key(i))) { return -1 - i; } i = i + 1; }\n"
        "Sort.sortStable<String>(a, n);\n"
        "i = 0;\n"
        "while (i < n) { if (!a[i].equals(D.key(i))) { return -200 - i; } i = i + 1; }\n"
        "return 1;"), 1);
}

// Lookups use fresh keys, so a tree ordered by address would miss them.
TEST(StringOrderingTests, redBlackTreeOfStringKeys) {
    EXPECT_EQ(runJit(
        "int32 n = 200;\n"
        "RedBlackTree<String, int64> t = heap RedBlackTree<String, int64>();\n"
        "int32 i = 0;\n"
        "while (i < n) { int32 v = D.scramble(i, n); t.put(#D.key(v), (int64) v); i = i + 1; }\n"
        "i = 0;\n"
        "while (i < n) { if (t.get(D.key(i)) != (int64) i) { return -1 - i; } i = i + 1; }\n"
        "if (!t.min().equals(D.key(0)) || !t.max().equals(D.key(n - 1))) { return -300; }\n"
        "return 1;"), 1);
}

TEST(StringOrderingTests, bPlusTreeOfStringKeys) {
    EXPECT_EQ(runJit(
        "int32 n = 400;\n"
        "BPlusTree<String, int64> t = heap BPlusTree<String, int64>();\n"
        "int32 i = 0;\n"
        "while (i < n) { int32 v = D.scramble(i, n); t.put(#D.key(v), (int64) v); i = i + 1; }\n"
        "i = 0;\n"
        "while (i < n) { if (t.get(D.key(i)) != (int64) i) { return -1 - i; } i = i + 1; }\n"
        "return 1;"), 1);
}
