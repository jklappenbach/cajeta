// Sort moves element titles with the elements (field-store-ownership spec 9.5): sorting an array
// that owns its Strings or objects keeps every element alive, frees each exactly once, and
// leaves borrowed elements borrowed.

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "import cajeta.collection.Sort;\n"
           "public class Cell {\n"
           "    public int64 n;\n"
           "    public Cell(int64 v) { this.n = v; }\n"
           "}\n"
           "public final class D {\n"
           "    public static #String key(int32 v) {\n"
           "        int8[] buf = Cajeta.allocBytes(20L);\n"
           "        int32 i = 19;\n"
           "        int32 x = v;\n"
           "        while (i >= 0) { buf[i] = (int8) (48 + (x - (x / 10) * 10)); x = x / 10; i = i - 1; }\n"
           "        return heap String(#buf, 20);\n"
           "    }\n"
           "    public static int32 scramble(int32 i, int32 n) { return (i * 37 + 11) - ((i * 37 + 11) / n) * n; }\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 400L) { String j #= D.key(7); Cell c = heap Cell(107800L); i = i + 1; }\n"
           "    }\n"
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

std::string sortStrings(const char* call) {
    return std::string(
        "int32 n = 100;\n"
        "String[] a = heap String[n];\n"
        "int32 i = 0;\n"
        "while (i < n) { a[i] #= D.key(D.scramble(i, n)); i = i + 1; }\n") +
        call +
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) {\n"
        "    if (!a[i].equals(D.key(i))) { return -1 - i; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;";
}

std::string sortCells(const char* call) {
    return std::string(
        "int32 n = 100;\n"
        "Cell[] a = heap Cell[n];\n"
        "int32 i = 0;\n"
        "while (i < n) { a[i] #= heap Cell((int64) D.scramble(i, n)); i = i + 1; }\n") +
        call +
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) {\n"
        "    if (a[i].n != (int64) i) { return -1 - i; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;";
}

}  // namespace

TEST(SortOwnershipTests, quicksortOfOwnedStrings) {
    EXPECT_EQ(runJit(sortStrings("Sort.sort<String>(a, n, (x, y) -> x.compareTo(y));\n")), 1);
}

TEST(SortOwnershipTests, stableSortOfOwnedStrings) {
    EXPECT_EQ(runJit(sortStrings("Sort.sortStable<String>(a, n, (x, y) -> x.compareTo(y));\n")), 1);
}

TEST(SortOwnershipTests, quicksortOfOwnedObjects) {
    EXPECT_EQ(runJit(sortCells(
        "Sort.sort<Cell>(a, n, (x, y) -> { if (x.n < y.n) { return -1; } if (x.n > y.n) { return 1; } return 0; });\n")), 1);
}

TEST(SortOwnershipTests, stableSortOfOwnedObjects) {
    EXPECT_EQ(runJit(sortCells(
        "Sort.sortStable<Cell>(a, n, (x, y) -> { if (x.n < y.n) { return -1; } if (x.n > y.n) { return 1; } return 0; });\n")), 1);
}

// Borrowed elements stay borrowed: sorting never frees what the array does not own.
TEST(SortOwnershipTests, sortingBorrowedStringsFreesNothing) {
    EXPECT_EQ(runJit(
        "int32 n = 50;\n"
        "String[] owners = heap String[n];\n"
        "String[] a = heap String[n];\n"
        "int32 i = 0;\n"
        "while (i < n) { owners[i] #= D.key(D.scramble(i, n)); i = i + 1; }\n"
        "i = 0;\n"
        "while (i < n) { a[i] = owners[i]; i = i + 1; }\n"
        "Sort.sortStable<String>(a, n, (x, y) -> x.compareTo(y));\n"
        "D.churn();\n"
        "i = 0;\n"
        "while (i < n) {\n"
        "    if (!a[i].equals(D.key(i))) { return -1 - i; }\n"
        "    if (owners[i].size() != 20) { return -100 - i; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;"), 1);
}
