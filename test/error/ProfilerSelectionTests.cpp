// cajeta-profiler Unit 10.4 — the instrumentation selection (spec §3.8-§3.12).
//
// The selection is a CODEGEN decision. Excluded code carries no probe to skip,
// which is what makes a narrow selection an overhead reduction rather than a
// display preference. These tests pin the membership rule itself; the IR-level
// "no probe was emitted" half lives in ProfilerInstrumentationTests, because
// "no probe emitted" and "probe emitted then ignored" are indistinguishable
// from anywhere downstream of codegen.
#include "gtest/gtest.h"

#include "cajeta/prof/ProfileSelection.h"

using cajeta::prof::ProfileSelection;

namespace {
    ProfileSelection parse(const std::string& text) {
        return ProfileSelection::parse(text);
    }
}

// ── the glob ──────────────────────────────────────────────────────────────

// `*` stays inside one name segment. This is the whole reason there are two
// wildcards: `dev.a.*` has to mean "the classes in dev.a", or a package
// selection silently drags in every subpackage.
TEST(ProfilerSelection, singleStarDoesNotCrossPackageBoundaries) {
    EXPECT_TRUE(ProfileSelection::matches("dev.a.*", "dev.a.Widget"));
    EXPECT_FALSE(ProfileSelection::matches("dev.a.*", "dev.a.sub.Widget"));
}

// `**` does cross them, so `dev.a.**` is "the package and everything under it".
TEST(ProfilerSelection, doubleStarCrossesPackageBoundaries) {
    EXPECT_TRUE(ProfileSelection::matches("dev.a.**", "dev.a.Widget"));
    EXPECT_TRUE(ProfileSelection::matches("dev.a.**", "dev.a.sub.deep.Widget"));
    EXPECT_FALSE(ProfileSelection::matches("dev.a.**", "dev.b.Widget"));
}

// A pattern with no wildcard is one exact class, and does not prefix-match.
TEST(ProfilerSelection, bareNameIsExact) {
    EXPECT_TRUE(ProfileSelection::matches("dev.a.Widget", "dev.a.Widget"));
    EXPECT_FALSE(ProfileSelection::matches("dev.a.Widget", "dev.a.WidgetImpl"));
    EXPECT_FALSE(ProfileSelection::matches("dev.a.Widget", "dev.a.sub.Widget"));
}

// A wildcard mid-segment, which is how you write "the Debug* helpers".
TEST(ProfilerSelection, starMatchesWithinASegment) {
    EXPECT_TRUE(ProfileSelection::matches("dev.a.Debug*", "dev.a.DebugPanel"));
    EXPECT_FALSE(ProfileSelection::matches("dev.a.Debug*", "dev.a.Panel"));
}

// ── the membership rule (§3.9) ────────────────────────────────────────────

// No directives at all: `--profiler=instrument` with no selection instruments
// everything, which is what makes the selection genuinely optional.
TEST(ProfilerSelection, emptySelectionSelectsEverything) {
    ProfileSelection sel = parse("");
    EXPECT_TRUE(sel.empty());
    EXPECT_TRUE(sel.selects("anything.At.All"));
}

// Include defines the universe: anything it does not name is out.
TEST(ProfilerSelection, includeDefinesTheUniverse) {
    ProfileSelection sel = parse("include dev.a.**\n");
    EXPECT_TRUE(sel.selects("dev.a.Widget"));
    EXPECT_FALSE(sel.selects("dev.b.Widget"));
}

// Exclude subtracts from it.
TEST(ProfilerSelection, excludeSubtractsFromTheUniverse) {
    ProfileSelection sel = parse("include dev.a.**\nexclude dev.a.sub.**\n");
    EXPECT_TRUE(sel.selects("dev.a.Widget"));
    EXPECT_FALSE(sel.selects("dev.a.sub.Widget"));
}

// With no include lines the universe is everything, so a file of pure
// excludes reads as "instrument the program except this".
TEST(ProfilerSelection, excludeOnlyMeansEverythingElse) {
    ProfileSelection sel = parse("exclude dev.a.Noisy\n");
    EXPECT_FALSE(sel.selects("dev.a.Noisy"));
    EXPECT_TRUE(sel.selects("dev.a.Quiet"));
    EXPECT_TRUE(sel.selects("dev.b.Anything"));
}

// §3.9's actual requirement, and the one a "first match wins" implementation
// fails: membership must not depend on which line came first.
TEST(ProfilerSelection, membershipDoesNotDependOnOrdering) {
    ProfileSelection a = parse("include dev.a.**\nexclude dev.a.Noisy\n");
    ProfileSelection b = parse("exclude dev.a.Noisy\ninclude dev.a.**\n");
    for (const char* n : {"dev.a.Noisy", "dev.a.Quiet", "dev.b.Other"}) {
        EXPECT_EQ(a.selects(n), b.selects(n)) << "ordering changed membership of " << n;
    }
    EXPECT_FALSE(a.selects("dev.a.Noisy"));
}

// The other half: nor on which pattern is more specific. An exact-name include
// does NOT win over a broad exclude — exclude always subtracts.
TEST(ProfilerSelection, membershipDoesNotDependOnSpecificity) {
    ProfileSelection sel = parse("include dev.a.Noisy\nexclude dev.a.**\n");
    EXPECT_FALSE(sel.selects("dev.a.Noisy"))
        << "a more specific include beat the exclude; §3.9 has ONE rule";
}

// ── the file ──────────────────────────────────────────────────────────────

TEST(ProfilerSelection, commentsAndBlankLinesAreIgnored) {
    ProfileSelection sel = parse(
        "# the hot path only\n"
        "\n"
        "   include dev.a.**   # everything under a\n"
        "\n");
    EXPECT_TRUE(sel.selects("dev.a.Widget"));
    EXPECT_FALSE(sel.selects("dev.b.Widget"));
}

// A bare pattern is an include, so the common case reads without the keyword.
TEST(ProfilerSelection, bareLineIsAnInclude) {
    ProfileSelection sel = parse("dev.a.**\n");
    EXPECT_TRUE(sel.selects("dev.a.Widget"));
    EXPECT_FALSE(sel.selects("dev.b.Widget"));
}

// A typo must not silently widen the selection. `inlcude dev.a.**` taken as a
// pattern would match nothing, the include set would be non-empty, and the
// build would instrument NOTHING while reporting success — which reads as a
// broken profiler rather than a broken file.
TEST(ProfilerSelection, misspelledKeywordIsReportedNotGuessed) {
    std::vector<std::string> errors;
    ProfileSelection sel = ProfileSelection::parse("inlcude dev.a.**\n", &errors);
    ASSERT_EQ(errors.size(), 1u) << "a misspelled keyword was accepted silently";
    EXPECT_NE(errors[0].find("inlcude"), std::string::npos)
        << "the diagnostic does not name the typo: " << errors[0];
    EXPECT_TRUE(sel.empty());
}

// §3.12 — the trace has to state the selection, so there is a canonical form.
// Canonical, not an echo: sorted and deduped, comments dropped.
TEST(ProfilerSelection, describeIsCanonicalNotAnEcho) {
    ProfileSelection a = parse("include dev.b.**\ninclude dev.a.**\nexclude dev.a.X\n");
    ProfileSelection b = parse("# same set, written differently\n"
                               "exclude dev.a.X\n"
                               "include dev.a.**\n"
                               "include dev.b.**\ninclude dev.a.**\n");
    EXPECT_EQ(a.describe(), b.describe());
    EXPECT_EQ(a.describe(), "include dev.a.**; include dev.b.**; exclude dev.a.X");
}

// An empty selection says so rather than rendering as the empty string, which
// a trace reader would have to interpret.
TEST(ProfilerSelection, describeOfAnEmptySelectionSaysAll) {
    EXPECT_EQ(parse("").describe(), "all");
}
