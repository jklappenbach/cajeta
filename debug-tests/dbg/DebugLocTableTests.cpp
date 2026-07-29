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

// ---- fast-debug-launch Unit 3: sparse replay + sidecar (plan 3.1.1/3.1.2) --
// A cached module's baked loc_ids must survive a relaunch: the sidecar stores
// (id, DbgLoc) pairs, setAt() replays them at their ORIGINAL indices (holes
// where nothing replayed), and add() continues past the max so a Stage-B
// dirty compile can never collide with replayed ids.

#include <filesystem>
#include <random>

namespace {
std::filesystem::path tempSidecarPath() {
    static std::mt19937_64 rng(std::random_device{}());
    return std::filesystem::temp_directory_path()
         / ("cajeta_dbgloc_test_" + std::to_string(rng()) + ".dbgloc");
}
} // namespace

TEST(DbgLocTable, SetAtReplaysSparseWithHoles) {
    DbgLocTable t;
    t.setAt(5, {"A.cajeta", 10, 2, "demo.A::main"});
    t.setAt(2, {"B.cajeta", 20, 4, "demo.B::run"});
    EXPECT_EQ(t.size(), 6u);  // 0..5, holes at 0,1,3,4

    EXPECT_EQ(t.at(5).file, "A.cajeta");
    EXPECT_EQ(t.at(5).line, 10);
    EXPECT_EQ(t.at(2).file, "B.cajeta");
    EXPECT_EQ(t.at(2).function, "demo.B::run");
    // A hole reads as an empty loc, never a crash.
    EXPECT_TRUE(t.at(0).file.empty());
    EXPECT_TRUE(t.at(3).file.empty());
}

TEST(DbgLocTable, AddContinuesPastMaxReplayedId) {
    DbgLocTable t;
    t.setAt(7, {"A.cajeta", 1, 1, "f"});
    int32_t next = t.add("A.cajeta", 2, 1, "f");
    EXPECT_EQ(next, 8);
    EXPECT_EQ(t.at(8).line, 2);
    // Replaying below the high-water mark never disturbs appended entries.
    t.setAt(3, {"B.cajeta", 9, 1, "g"});
    EXPECT_EQ(t.at(8).line, 2);
    EXPECT_EQ(t.add("A.cajeta", 3, 1, "f"), 9);
}

TEST(DbgLocTable, IdsForLineSkipsHoles) {
    DbgLocTable t;
    t.setAt(1, {"C.cajeta", 5, 1, "f"});
    t.setAt(4, {"C.cajeta", 5, 9, "f"});
    auto ids = t.idsForLine("C.cajeta", 5);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 4);
    // Holes have line 0 and empty file; neither may ever match.
    EXPECT_TRUE(t.idsForLine("", 0).empty());
}

TEST(DbgLocSidecar, RoundTripsSparseTable) {
    DbgLocTable t;
    t.add("A.cajeta", 10, 2, "demo.A::main");                    // 0
    t.add("A.cajeta", 11, 2, "demo.A::main");                    // 1
    t.setAt(5, {"weird\tname.cajeta", 3, 1, "demo.W::f"});       // hole 2..4
    t.add("A.cajeta", 12, 2, "");   // 6 — EMPTY function (clinit-style entry)
    auto path = tempSidecarPath();
    ASSERT_TRUE(cajeta::dbg::writeDbgLocSidecar(path.string(), t));

    DbgLocTable back;
    ASSERT_TRUE(cajeta::dbg::loadDbgLocSidecar(path.string(), back));
    ASSERT_EQ(back.size(), t.size());
    for (size_t id = 0; id < t.size(); ++id) {
        EXPECT_EQ(back.at((int32_t) id).file, t.at((int32_t) id).file);
        EXPECT_EQ(back.at((int32_t) id).line, t.at((int32_t) id).line);
        EXPECT_EQ(back.at((int32_t) id).col, t.at((int32_t) id).col);
        EXPECT_EQ(back.at((int32_t) id).function, t.at((int32_t) id).function);
    }
    auto ids = back.idsForLine("A.cajeta", 10);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 0);
    // The escaped tab survived.
    EXPECT_EQ(back.at(5).file, "weird\tname.cajeta");
    std::filesystem::remove(path);
}

TEST(DbgLocSidecar, LoadMissingFileFailsCleanly) {
    DbgLocTable t;
    EXPECT_FALSE(cajeta::dbg::loadDbgLocSidecar(
        tempSidecarPath().string(), t));
    EXPECT_TRUE(t.empty());
}
