// Tests for the --diag-hints=on feature (CompilerModes.md § --diag-hints).
//
// First slice: "did you mean..." suggestions on @ToString(of={...}) when
// the named field doesn't exist. Direct unit tests cover the
// Diagnostics utility (Levenshtein + pickSimilar + formatDidYouMean);
// JIT integration tests confirm the wiring through the compiler
// honors the diagHints flag.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

// ----- Diagnostics utility tests -------------------------------------------

TEST(DiagHintsUnitTests, levenshteinKnownDistances) {
    EXPECT_EQ(cajeta::levenshteinDistance("foo", "foo"), 0);
    EXPECT_EQ(cajeta::levenshteinDistance("foo", "foa"), 1);  // 1 subst
    EXPECT_EQ(cajeta::levenshteinDistance("foo", "fo"),  1);  // 1 del
    EXPECT_EQ(cajeta::levenshteinDistance("foo", "fooo"), 1); // 1 ins
    EXPECT_EQ(cajeta::levenshteinDistance("kitten", "sitting"), 3);
    EXPECT_EQ(cajeta::levenshteinDistance("", "abc"), 3);
    EXPECT_EQ(cajeta::levenshteinDistance("abc", ""), 3);
    EXPECT_EQ(cajeta::levenshteinDistance("", ""), 0);
}

TEST(DiagHintsUnitTests, pickSimilarReturnsClosestMatches) {
    std::vector<std::string> candidates = {"name", "namee", "nme", "address", "zzz"};
    auto out = cajeta::pickSimilar("nam", candidates);
    // Distance from "nam":
    //   "name"    → 1 (insert e)
    //   "nme"     → 2 (delete a, insert e at end OR subst a→m, subst m→e)
    //   "namee"   → 2 (insert e, insert e)
    //   "address" → ≥4 (filtered out)
    //   "zzz"     → 3 (filtered out)
    // Sort: distance ascending, then alphabetical.
    //   1. "name"   dist=1
    //   2. "namee"  dist=2 ("namee" < "nme" alphabetically)
    //   3. "nme"    dist=2
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "name");
    EXPECT_EQ(out[1], "namee");
    EXPECT_EQ(out[2], "nme");
}

TEST(DiagHintsUnitTests, pickSimilarRespectsMaxDistance) {
    std::vector<std::string> candidates = {"alpha", "beta", "gamma"};
    auto out = cajeta::pickSimilar("zzzzz", candidates, /*maxDistance=*/2);
    EXPECT_TRUE(out.empty());
}

TEST(DiagHintsUnitTests, pickSimilarRespectsMaxSuggestions) {
    std::vector<std::string> candidates = {"foo", "fooa", "foob", "fooc", "food"};
    auto out = cajeta::pickSimilar("foo", candidates,
                                    /*maxDistance=*/2,
                                    /*maxSuggestions=*/2);
    EXPECT_EQ(out.size(), 2u);
}

TEST(DiagHintsUnitTests, pickSimilarSkipsExactMatch) {
    std::vector<std::string> candidates = {"foo", "fooa", "bar"};
    auto out = cajeta::pickSimilar("foo", candidates);
    // "foo" itself is exact-match → skipped; "fooa" (dist 1) wins.
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "fooa");
}

TEST(DiagHintsUnitTests, formatDidYouMeanEmpty) {
    EXPECT_EQ(cajeta::formatDidYouMean({}), "");
}

TEST(DiagHintsUnitTests, formatDidYouMeanSingle) {
    EXPECT_EQ(cajeta::formatDidYouMean({"foo"}),
              " did you mean `foo`?");
}

TEST(DiagHintsUnitTests, formatDidYouMeanMultiple) {
    EXPECT_EQ(cajeta::formatDidYouMean({"foo", "bar", "baz"}),
              " did you mean one of: `foo`, `bar`, `baz`?");
}

// ----- JIT integration tests -----------------------------------------------

// With diagHints on (the default for the Debug compile flags the JIT
// inherits), the @ToString(of={"naem"}) error names a typo'd field and
// the message includes a "did you mean..." suggestion mentioning the
// real field `name`. Use a distant second field so only `name`
// qualifies under maxDistance=2 — gives us the singular-form path.
TEST(DiagHintsTests, toStringOfUnknownFieldGetsDidYouMean) {
    auto src =
        "package test;\n"
        "@ToString(of={\"naem\"}) public class P {\n"
        "    public int32 name;\n"
        "    public int32 totalLifetimeSpentReading;\n"
        "    public P(int32 n, int32 t) {\n"
        "        this.name = n;\n"
        "        this.totalLifetimeSpentReading = t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD");
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("did you mean `name`"), std::string::npos)
            << "actual message: " << msg;
    }
}

// Diagnostics produces multiple suggestions when several candidates are
// equally close. Field `nam` is within distance 2 of both `name` and
// `name1`/`name2`, so the multi-form should fire.
TEST(DiagHintsTests, toStringOfUnknownFieldOffersMultipleCandidates) {
    auto src =
        "package test;\n"
        "@ToString(of={\"nam\"}) public class P {\n"
        "    public int32 name;\n"
        "    public int32 name1;\n"
        "    public int32 name2;\n"
        "    public P() { this.name = 0; this.name1 = 0; this.name2 = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD");
        EXPECT_NE(std::string(e.getMessage()).find("did you mean one of:"),
                  std::string::npos)
            << "actual message: " << e.getMessage();
        EXPECT_NE(std::string(e.getMessage()).find("`name`"),
                  std::string::npos);
    }
}

// No close candidate → no "did you mean" suffix. Error fires with the
// base message only.
TEST(DiagHintsTests, toStringOfUnknownFieldNoCloseMatchOmitsHint) {
    auto src =
        "package test;\n"
        "@ToString(of={\"zzzzz\"}) public class P {\n"
        "    public int32 name;\n"
        "    public P(int32 n) { this.name = n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD");
        EXPECT_EQ(std::string(e.getMessage()).find("did you mean"),
                  std::string::npos)
            << "should NOT contain a suggestion for distant input; got: "
            << e.getMessage();
    }
}
