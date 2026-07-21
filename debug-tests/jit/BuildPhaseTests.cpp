//
// fast-debug-launch Unit 1 — launch-phase instrumentation (plan 1.1.1/1.1.2).
// buildJit fills a JitBuildPhases record (surfaced through JitRunResult) and
// drives an optional onProgress callback at phase boundaries and per-source
// during parse. These pin the record's invariants and the callback's ordering
// so Unit 2 can hang DAP output events on the same seam.
//
#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "../TempProgram.h"
#include "cajeta/jit/CajetaJitHost.h"

using cajeta::debugtest::TempProgram;
using cajeta::jit::JitRunOptions;
using cajeta::jit::JitRunResult;
using cajeta::jit::runJit;

namespace {

const char* kTwoClassA =
    "package demo;\n"
    "public class PhaseA {\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"
    "        int32 b = 7;\n"
    "        return a * b;\n"
    "    }\n"
    "}\n";

const char* kTwoClassB =
    "package demo;\n"
    "public class PhaseB {\n"
    "    public static int32 helper() {\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

} // namespace

// 1.1.1 — the phase-timing record is filled: every phase non-negative, the
// wall total positive and no smaller than the sum of its disjoint sub-phases
// (they are consecutive segments of the same wall interval).
TEST(JitBuildPhases, RecordFilledAndInternallyConsistent) {
    TempProgram p("demo", "PhaseA.cajeta", kTwoClassA);

    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.PhaseA.main";

    JitRunResult result;
    ASSERT_EQ(runJit(opts, &result), 42);

    const auto& ph = result.phases;
    EXPECT_GE(ph.collectSeconds, 0.0);
    EXPECT_GE(ph.parseSeconds, 0.0);
    EXPECT_GE(ph.codegenStdlibSeconds, 0.0);
    EXPECT_GE(ph.codegenUserSeconds, 0.0);
    EXPECT_GE(ph.finalizeSeconds, 0.0);
    EXPECT_GE(ph.mergeSeconds, 0.0);
    EXPECT_GE(ph.jitSeconds, 0.0);

    // Real work happened in these phases; a zero here means the timer never
    // ran, not that the phase was instant.
    EXPECT_GT(ph.parseSeconds, 0.0);
    EXPECT_GT(ph.totalSeconds, 0.0);
    EXPECT_GT(ph.jitSeconds, 0.0);

    const double sum = ph.collectSeconds + ph.parseSeconds
                     + ph.codegenStdlibSeconds + ph.codegenUserSeconds
                     + ph.finalizeSeconds + ph.mergeSeconds + ph.jitSeconds;
    EXPECT_LE(sum, ph.totalSeconds + 1e-6);
}

// 1.1.2 — the progress callback fires, phases arrive in pipeline order, and
// per-source parse events count 1..total with total = the real source count.
TEST(JitBuildPhases, ProgressCallbackFiresInOrderWithSourceCounts) {
    TempProgram p("demo", "PhaseA.cajeta", kTwoClassA);
    {
        // Second file in the same root/package: total must be 2.
        std::ofstream out(p.root / "demo" / "PhaseB.cajeta");
        out << kTwoClassB;
    }

    struct Event {
        std::string phase;
        std::string detail;
        int current;
        int total;
    };
    std::vector<Event> events;
    std::mutex mu;

    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.PhaseA.main";
    opts.onProgress = [&](const std::string& phase, const std::string& detail,
                          int current, int total) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back({phase, detail, current, total});
    };

    JitRunResult result;
    ASSERT_EQ(runJit(opts, &result), 42);
    ASSERT_FALSE(events.empty());

    // Distinct phase labels, in first-appearance order, must be a subsequence
    // of the canonical pipeline order.
    const std::vector<std::string> canonical = {
        "collect", "parse", "codegen", "finalize", "merge", "jit"};
    std::vector<std::string> seen;
    for (const auto& e : events)
        if (seen.empty() || seen.back() != e.phase)
            if (std::find(seen.begin(), seen.end(), e.phase) == seen.end())
                seen.push_back(e.phase);
    size_t pos = 0;
    for (const auto& phase : seen) {
        auto it = std::find(canonical.begin() + pos, canonical.end(), phase);
        ASSERT_NE(it, canonical.end())
            << "phase '" << phase << "' out of order or unknown";
        pos = it - canonical.begin();
    }
    // The load-bearing phases actually reported.
    EXPECT_NE(std::find(seen.begin(), seen.end(), "parse"), seen.end());
    EXPECT_NE(std::find(seen.begin(), seen.end(), "jit"), seen.end());

    // Per-source parse events: total == 2, current covers 1..2 in order, and
    // the detail names the source file.
    std::vector<Event> parseEvents;
    for (const auto& e : events)
        if (e.phase == "parse" && e.total > 0) parseEvents.push_back(e);
    ASSERT_EQ(parseEvents.size(), 2u);
    EXPECT_EQ(parseEvents[0].current, 1);
    EXPECT_EQ(parseEvents[1].current, 2);
    for (const auto& e : parseEvents) {
        EXPECT_EQ(e.total, 2);
        EXPECT_NE(e.detail.find(".cajeta"), std::string::npos);
    }
}
