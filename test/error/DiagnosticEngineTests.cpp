// diagnostic-engine (collect-and-continue): the engine accumulates recoverable
// diagnostics, then finalizes them sorted-by-span, deduped, and capped.

#include <gtest/gtest.h>

#include <string>

#include "cajeta/error/DiagnosticEngine.h"

using cajeta::DiagnosticEngine;

TEST(DiagnosticEngine, AccumulatesAndTracksErrors) {
    DiagnosticEngine e;
    EXPECT_FALSE(e.hasErrors());
    e.report("warning", "W1", "unused", "/a", 1, 1);
    EXPECT_FALSE(e.hasErrors());          // warnings don't set the error flag
    e.report("note", "N1", "fyi", "/a", 2, 1);
    EXPECT_FALSE(e.hasErrors());
    e.report("error", "E1", "boom", "/a", 3, 1);
    EXPECT_TRUE(e.hasErrors());
    EXPECT_EQ(e.count(), 3u);
}

TEST(DiagnosticEngine, FinalizeSortsBySpanAndDedups) {
    DiagnosticEngine e;
    e.report("error", "E", "b", "/a.cajeta", 10, 3);
    e.report("error", "E", "a", "/a.cajeta", 4, 9);
    e.report("error", "E", "a", "/a.cajeta", 4, 9);   // exact dup → dropped
    e.report("error", "E", "c", "/a.cajeta", 4, 2);
    auto out = e.finalize();
    ASSERT_EQ(out.size(), 3u);                          // one dup removed
    // Sorted by (file, line, column): (4,2), (4,9), (10,3).
    EXPECT_EQ(out[0].line, 4);  EXPECT_EQ(out[0].column, 2);
    EXPECT_EQ(out[1].line, 4);  EXPECT_EQ(out[1].column, 9);
    EXPECT_EQ(out[2].line, 10); EXPECT_EQ(out[2].column, 3);
}

TEST(DiagnosticEngine, CapsWithTrailingNote) {
    DiagnosticEngine e;
    const int n = DiagnosticEngine::CAP + 5;
    for (int i = 0; i < n; i++)
        e.report("error", "E", "x", "/a", i + 1, 1);
    auto out = e.finalize();
    // CAP diagnostics + one "…and N more" note.
    ASSERT_EQ(out.size(), static_cast<size_t>(DiagnosticEngine::CAP) + 1);
    EXPECT_EQ(out.back().severity, "note");
    EXPECT_NE(out.back().message.find("more"), std::string::npos);
}

TEST(DiagnosticEngine, SuppressedCollectsNothing) {
    DiagnosticEngine e(/*suppressed=*/true);
    e.report("error", "E", "boom", "/a", 1, 1);
    EXPECT_FALSE(e.hasErrors());
    EXPECT_EQ(e.count(), 0u);
    EXPECT_TRUE(e.finalize().empty());
}

TEST(DiagnosticEngine, ActiveAccessorInstallsAndClears) {
    EXPECT_EQ(DiagnosticEngine::active(), nullptr);
    DiagnosticEngine e;
    DiagnosticEngine::setActive(&e);
    EXPECT_EQ(DiagnosticEngine::active(), &e);
    DiagnosticEngine::setActive(nullptr);
    EXPECT_EQ(DiagnosticEngine::active(), nullptr);
}
