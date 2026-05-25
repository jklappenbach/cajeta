// Task #41 — side-channel correlation tests for anyMatch / findAny
// dispatch on parallel streams.
//
// Existing dispatch tests only verify the returned value satisfies
// the static predicate. They wouldn't catch a "wrong-but-valid"
// answer — e.g. driver reads stale slot data and returns 75 when the
// worker that short-circuited actually matched 60. Both 60 and 75
// pass `(x) -> x > 50`, so the bug hides.
//
// Each test below installs a side channel — a shared Probe the
// predicate-helper mutates via direct field writes — so the
// predicate's `true`-returns are observable from outside the
// terminal. The terminal's return value is then cross-checked
// against the side channel, not just against the static predicate.
//
// Observable surface (kept narrow on purpose):
//   - bumps: how many times the predicate returned true
//   - last:  the last value the predicate observed as a match
//
// Combined with single-match sources, these together detect:
//   (a) terminal returns a value the predicate never matched
//       (returned != last AND bumps == 1)
//   (b) terminal returns true/non-empty without any predicate match
//       (bumps == 0)
//   (c) terminal returns false/empty despite a predicate match
//       (bumps > 0 but terminal said "no")
//
// Note on idiom: predicate logic is hoisted into a static helper
// (returns boolean, side-effects the Probe) and the lambda body
// stays a single-expression call to that helper. Equivalent to a
// multi-statement block-body lambda that does the same work
// inline; the helper form just isolates the side channel mechanics
// from the lambda-capture mechanics so a test failure points at
// the dispatch path, not at lambda lowering. Lambda array-field
// writes (`p.log[idx] = x`) still trip an LLVM alloca-cast
// assertion in unrelated code, so the side channel uses scalar
// fields only.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        return jit->lookup<int32_t (*)()>("run")();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                      << ": " << e.getMessage();
        return -1;
    }
}

// The shared Probe class + a few predicate helpers prepended to
// every test's Cajeta source. `matchEq(p, x, target)` is the
// single-match shape; `matchGt9999(p, x)` is the always-false shape.
constexpr const char* kProbePrologue =
    "package test;\n"
    "public class Probe {\n"
    "    public int32 bumps;\n"
    "    public int32 last;\n"
    "    public Probe() {\n"
    "        this.bumps = 0;\n"
    "        this.last = -1;\n"
    "    }\n"
    "}\n";

// Each test's `public final class D { ... }` defines its own static
// helper(s) of shape `boolean matchAndNote(Probe p, int32 x)`; the
// lambda is then just `(x) -> matchAndNote(p, x)`.

} // namespace

// findFirst over a UNIQUE match — only x == 73 satisfies the
// predicate. The side channel records: the predicate fired true
// exactly once (bumps >= 1) and the last observed match value was
// 73. The terminal's returned value must equal that recorded value.
//
// Bug shapes caught:
//   - returned value is not 73 (off-by-one / stale slot)
//   - terminal returns 73 but predicate never fired (bumps == 0)
//   - terminal returns 73 but probe.last != 73 (predicate ran on a
//     different value but somehow returned 73 anyway)
TEST(ParallelDispatchCorrelationTests, findFirstUniqueMatchReturnedValueWasObserved) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIf73(Probe p, int32 x) {\n"
        "        if (x == 73) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> noteIf73(p, x));\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        int32 v = o.get();\n"
        "        if (p.bumps < 1) { return -2; }\n"
        "        if (p.last != v) { return -3; }\n"
        "        return v;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "findFirst returned empty Optional";
    EXPECT_NE(r, -2) << "predicate never fired but findFirst returned a value";
    EXPECT_NE(r, -3) << "returned value disagrees with predicate's last-observed value";
    EXPECT_EQ(r, 73);
}

// findFirst with NO match — predicate never returns true. The
// terminal must return empty Optional and the side channel must
// register zero bumps. Catches: a phantom-match bug where the driver
// fabricates a return value without the predicate ever firing.
TEST(ParallelDispatchCorrelationTests, findFirstEmptyImpliesPredicateNeverMatched) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIfHuge(Probe p, int32 x) {\n"
        "        if (x > 9999) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> noteIfHuge(p, x));\n"
        "        if (o.isPresent()) { return -1; }\n"
        "        if (p.bumps != 0) { return -2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "findFirst returned a value when no element satisfies the predicate";
    EXPECT_NE(r, -2) << "predicate fired true despite no element satisfying it";
    EXPECT_EQ(r, 0);
}

// findFirst through a filter chain with unique match — only one
// even value (50) lies in the predicate's accept set. Validates
// that the cloneChainOver(piece) rebuild preserves correlation
// between predicate observation and returned value. Catches: a bug
// where the driver returns a pre-filter slot value (e.g. 49, 51)
// that bypasses both the filter and the predicate-true path.
TEST(ParallelDispatchCorrelationTests, findFirstThroughFilterUniqueMatchObserved) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIf50(Probe p, int32 x) {\n"
        "        if (x == 50) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        // Filter to evens, predicate matches x == 50.\n"
        "        Optional<int32> o = xs.stream()\n"
        "                              .filter((x) -> x % 2 == 0)\n"
        "                              .parallel()\n"
        "                              .findFirst((x) -> noteIf50(p, x));\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        int32 v = o.get();\n"
        "        if (p.bumps < 1) { return -2; }\n"
        "        if (p.last != v) { return -3; }\n"
        "        if ((v % 2) != 0) { return -4; }\n"
        "        return v;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "findFirst returned empty through filter on a source containing 50";
    EXPECT_NE(r, -2) << "predicate never fired despite findFirst returning a value";
    EXPECT_NE(r, -3) << "returned value disagrees with predicate's last-observed value";
    EXPECT_NE(r, -4) << "returned value bypassed the filter (raw pre-filter element)";
    EXPECT_EQ(r, 50);
}

// anyMatch true ⇒ predicate must have returned true at least once.
// Side channel: bump counter + last-observed. Catches: a
// "true-without-evidence" dispatch bug where the driver returns
// true without any worker actually matching (e.g. uninitialised
// result flag).
TEST(ParallelDispatchCorrelationTests, anyMatchTrueImpliesPredicateMatched) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIf42(Probe p, int32 x) {\n"
        "        if (x == 42) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        boolean hit = xs.stream().parallel()\n"
        "                        .anyMatch((x) -> noteIf42(p, x));\n"
        "        if (!hit) { return -1; }\n"
        "        if (p.bumps < 1) { return -2; }\n"
        "        if (p.last != 42) { return -3; }\n"
        "        return p.bumps;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "anyMatch returned false on a source containing 42";
    EXPECT_NE(r, -2) << "anyMatch true but predicate never returned true";
    EXPECT_NE(r, -3) << "predicate observed something other than the only matching value 42";
    EXPECT_GE(r, 1);
}

// anyMatch false ⇒ predicate must NEVER have returned true. Catches:
// a dispatch bug that drops a worker's true-signal (driver returns
// false despite at least one worker matching).
TEST(ParallelDispatchCorrelationTests, anyMatchFalseImpliesPredicateNeverMatched) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIfHuge(Probe p, int32 x) {\n"
        "        if (x > 9999) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        boolean hit = xs.stream().parallel()\n"
        "                        .anyMatch((x) -> noteIfHuge(p, x));\n"
        "        if (hit) { return -1; }\n"
        "        if (p.bumps != 0) { return -2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "anyMatch returned true on a source with no matches";
    EXPECT_NE(r, -2) << "predicate recorded a match but anyMatch returned false";
    EXPECT_EQ(r, 0);
}

// anyMatch through filter chain — same correlation guarantees as
// the direct-head version, but the share gets rebuilt over each
// split via cloneChainOver. Validates correlation survives the
// chain-walk path.
TEST(ParallelDispatchCorrelationTests, anyMatchThroughFilterCorrelation) {
    std::string src = kProbePrologue;
    src +=
        "public final class D {\n"
        "    public static boolean noteIf50(Probe p, int32 x) {\n"
        "        if (x == 50) {\n"
        "            p.bumps = p.bumps + 1;\n"
        "            p.last = x;\n"
        "            return true;\n"
        "        }\n"
        "        return false;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Probe p = new Probe();\n"
        "        // After filter (evens), 50 is the unique match.\n"
        "        boolean hit = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .anyMatch((x) -> noteIf50(p, x));\n"
        "        if (!hit) { return -1; }\n"
        "        if (p.bumps < 1) { return -2; }\n"
        "        if (p.last != 50) { return -3; }\n"
        "        return p.bumps;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    EXPECT_NE(r, -1) << "anyMatch returned false through filter on a source containing 50";
    EXPECT_NE(r, -2) << "anyMatch true but predicate never returned true";
    EXPECT_NE(r, -3) << "predicate observed something other than the only matching value 50";
    EXPECT_GE(r, 1);
}
