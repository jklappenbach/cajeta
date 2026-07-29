// Tests for the fuzzy skill matcher.
// See src/cajeta/buildtool/skill/SkillMatcher.h and
// specs/archive/skill-discovery-spec.md §3.5 (plan unit D.4a).

#include "cajeta/buildtool/skill/SkillMatcher.h"

#include "cajeta/buildtool/skill/SkillDocument.h"
#include "cajeta/buildtool/skill/SkillIndex.h"

#include <gtest/gtest.h>

#include <string>

using cajeta::buildtool::skill::matchSkills;
using cajeta::buildtool::skill::MatchOptions;
using cajeta::buildtool::skill::MatchSource;
using cajeta::buildtool::skill::SkillDocument;
using cajeta::buildtool::skill::SkillIndex;
using cajeta::buildtool::skill::SkillMatch;

namespace {

    SkillDocument doc(std::string id, std::vector<std::string> appliesTo,
                      std::string title = "") {
        SkillDocument d;
        d.id = std::move(id);
        d.appliesTo = std::move(appliesTo);
        d.title = std::move(title);
        d.body = "b";
        return d;
    }

    SkillIndex buildIndex(std::vector<SkillDocument> docs) {
        auto e = SkillIndex::build(docs);
        EXPECT_TRUE((bool)e);
        return e ? std::move(*e) : SkillIndex{};
    }

    const SkillMatch* findKey(const std::vector<SkillMatch>& v, llvm::StringRef k) {
        for (const auto& m : v)
            if (m.key == k) return &m;
        return nullptr;
    }

    SkillIndex sample() {
        return buildIndex({
            doc("file-open", {"cajeta/io/File", "cajeta/io/File.open"}, "Opening files"),
            doc("socket", {"cajeta/net/Socket"}, "Network sockets"),
            doc("linear", {"cajeta/torch/nn/Linear"}, "Linear layers"),
        });
    }

} // namespace

// D.4a.1 — a single-segment name typo resolves to the right name.
TEST(SkillMatcherTests, singleSegmentTypoResolves) {
    auto r = matchSkills("cajeta/io/Fiel.open", sample());
    const auto* m = findKey(r, "cajeta/io/File.open");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->source, MatchSource::Name);
    EXPECT_EQ(m->distance, 1);
    ASSERT_FALSE(m->ids.empty());
    EXPECT_EQ(m->ids[0], "file-open");
}

// D.4a.1 — adjacent transposition resolves with distance 1 (Damerau, not plain
// Levenshtein, which would score Flie→File as 2).
TEST(SkillMatcherTests, transpositionIsDistanceOne) {
    auto r = matchSkills("cajeta/io/Flie", sample());
    const auto* m = findKey(r, "cajeta/io/File");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->distance, 1);
}

// D.4a.1 — a misspelled title resolves to that title.
TEST(SkillMatcherTests, titleTypoResolves) {
    auto r = matchSkills("Openin files", sample());
    const auto* m = findKey(r, "Opening files");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->source, MatchSource::Title);
    ASSERT_FALSE(m->ids.empty());
    EXPECT_EQ(m->ids[0], "file-open");
}

// D.4a.1 — segment-aware: a query with a different segment count does not match
// (no nonsense cross-segment fuzzing).
TEST(SkillMatcherTests, segmentAwareNoCrossSegment) {
    // "Fileopen" merges the File.open segments → 3 segments vs the key's 4.
    auto r = matchSkills("cajeta/io/Fileopen", sample());
    EXPECT_EQ(findKey(r, "cajeta/io/File.open"), nullptr);
}

// D.4a.1 — a query beyond the length-scaled threshold returns no garbage.
TEST(SkillMatcherTests, thresholdRejectsGarbage) {
    auto r = matchSkills("cajeta/io/Xyzzyq", sample());
    EXPECT_EQ(findKey(r, "cajeta/io/File"), nullptr);
    EXPECT_EQ(findKey(r, "cajeta/io/File.open"), nullptr);
}

// D.4a.1 — exact mode bypasses fuzzy.
TEST(SkillMatcherTests, exactModeBypassesFuzzy) {
    MatchOptions exact;
    exact.exact = true;
    EXPECT_TRUE(matchSkills("cajeta/io/Fiel.open", sample(), exact).empty());
    auto r = matchSkills("cajeta/io/File", sample(), exact);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.front().distance, 0);
    EXPECT_EQ(r.front().key, "cajeta/io/File");
}

// D.4a.1 — ranking: exact outranks fuzzy; smaller distance first.
TEST(SkillMatcherTests, rankingExactBeforeFuzzy) {
    auto idx = buildIndex({
        doc("a", {"cajeta/io/File"}),
        doc("b", {"cajeta/io/Bile"}),
    });
    auto r = matchSkills("cajeta/io/File", idx);
    ASSERT_GE(r.size(), 2u);
    EXPECT_EQ(r.front().key, "cajeta/io/File");
    EXPECT_EQ(r.front().distance, 0);
    const auto* bile = findKey(r, "cajeta/io/Bile");
    ASSERT_NE(bile, nullptr);
    EXPECT_EQ(bile->distance, 1);
}

// D.4a.1 — ranking: a Name match outranks a Title match at equal distance.
TEST(SkillMatcherTests, rankingNameBeforeTitleAtEqualDistance) {
    auto idx = buildIndex({
        doc("n", {"report"}),               // a single-segment name "report"
        doc("t", {"other/Thing"}, "report"), // a title "report"
    });
    auto r = matchSkills("report", idx);
    ASSERT_GE(r.size(), 2u);
    // Both are exact (distance 0); the Name one must come first.
    EXPECT_EQ(r.front().distance, 0);
    EXPECT_EQ(r.front().source, MatchSource::Name);
}

// D.4a.3 — determinism: identical query → identical ranked output.
TEST(SkillMatcherTests, deterministic) {
    auto idx = sample();
    auto a = matchSkills("cajeta/io/Fiel.open", idx);
    auto b = matchSkills("cajeta/io/Fiel.open", idx);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].key, b[i].key);
        EXPECT_EQ(a[i].distance, b[i].distance);
        EXPECT_EQ(a[i].source, b[i].source);
    }
}
