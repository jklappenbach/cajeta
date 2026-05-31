//
// Pure-function tests for the debug location table (CP2). No codegen or JIT —
// microsecond tests pinning the loc_id assignment + (file,line) lookup the
// safepoint/breakpoint machinery relies on.
//
#include <gtest/gtest.h>

#include "cajeta/dbg/DebugLocTable.h"

using cajeta::dbg::DbgLocTable;

TEST(DbgLocTable, AssignsSequentialIds) {
    DbgLocTable t;
    EXPECT_EQ(t.add("A.cajeta", 10, 4, "demo.A::main"), 0);
    EXPECT_EQ(t.add("A.cajeta", 11, 4, "demo.A::main"), 1);
    EXPECT_EQ(t.add("A.cajeta", 12, 4, "demo.A::main"), 2);
    EXPECT_EQ(t.size(), 3u);
}

TEST(DbgLocTable, RoundTripsLocation) {
    DbgLocTable t;
    int32_t id = t.add("B.cajeta", 42, 8, "demo.B::run");
    const auto& loc = t.at(id);
    EXPECT_EQ(loc.file, "B.cajeta");
    EXPECT_EQ(loc.line, 42);
    EXPECT_EQ(loc.col, 8);
    EXPECT_EQ(loc.function, "demo.B::run");
}

TEST(DbgLocTable, DoesNotDedupSameLine) {
    DbgLocTable t;
    // Two statements on the same line get distinct ids.
    int32_t a = t.add("C.cajeta", 5, 1, "f");
    int32_t b = t.add("C.cajeta", 5, 9, "f");
    EXPECT_NE(a, b);
    EXPECT_EQ(t.size(), 2u);
}

TEST(DbgLocTable, IdsForLineMatchesFileAndLine) {
    DbgLocTable t;
    t.add("C.cajeta", 5, 1, "f");   // 0
    t.add("C.cajeta", 6, 1, "f");   // 1
    t.add("C.cajeta", 5, 9, "f");   // 2  (same line as 0)
    t.add("Other.cajeta", 5, 1, "g"); // 3 (same line, different file)

    auto ids = t.idsForLine("C.cajeta", 5);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 0);
    EXPECT_EQ(ids[1], 2);

    EXPECT_TRUE(t.idsForLine("C.cajeta", 99).empty());
    EXPECT_EQ(t.idsForLine("Other.cajeta", 5).size(), 1u);
}

TEST(DbgLocTable, ClearResets) {
    DbgLocTable t;
    t.add("A.cajeta", 1, 1, "f");
    EXPECT_FALSE(t.empty());
    t.clear();
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(t.size(), 0u);
    // ids restart after clear
    EXPECT_EQ(t.add("A.cajeta", 1, 1, "f"), 0);
}
