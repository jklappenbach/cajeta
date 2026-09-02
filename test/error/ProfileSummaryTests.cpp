// cajeta-profiler 14.4 — `cajeta profile summary`, the per-kernel table.
//
// The common question about a GPU run is "which kernel cost what". It is a
// QUERY, not a picture, and answering it meant opening the tool window or
// hand-writing SQL against Perfetto's trace_processor_shell — a separate
// download the toolchain does not ship (Julian, 2026-09-02).
//
// The summarizer is asserted on NUMBERS, not on formatting: a test that
// matched the rendered table would fail on a column-width change and pass on
// a wrong total.
#include "gtest/gtest.h"

#include "cajeta/prof/TraceSummary.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using cajeta::prof::Summary;
using cajeta::prof::SummaryOptions;

namespace {

std::string sourceRoot() {
    const char* env = std::getenv("CAJETA_SOURCE_ROOT");
    if (env && *env) return env;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

// The plugin's fixtures are the only checked-in traces, and amdgpu.pftrace is
// the one with device queues. Reused rather than minting a second fixture: a
// second copy of a trace is a second thing to keep in step with the writer.
std::string amdgpuTrace() {
    return sourceRoot() + "/ide-plugins/idea/src/test/resources/profiler/amdgpu.pftrace";
}
std::string hostOnlyTrace() {
    return sourceRoot() + "/ide-plugins/idea/src/test/resources/profiler/tour.pftrace";
}

bool exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

const cajeta::prof::KernelStat* row(const Summary& s, const std::string& name) {
    for (const auto& r : s.rows) if (r.name == name) return &r;
    return nullptr;
}

} // namespace

// 14.4.1 — per-kernel count / total / average.
TEST(ProfileSummary, totalsDeviceWorkPerKernel) {
    if (!exists(amdgpuTrace())) GTEST_SKIP() << "fixture unavailable";
    Summary s;
    std::string err;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), SummaryOptions{}, &s, &err)) << err;
    ASSERT_TRUE(s.sawAnyTrack) << "the amdgpu fixture must carry device queues";
    ASSERT_FALSE(s.rows.empty()) << "device queues but no kernel slices";

    for (const auto& r : s.rows) {
        EXPECT_GT(r.count, 0) << r.name;
        EXPECT_GE(r.totalNs, 0) << r.name;
        // The average is derived, so it must be consistent with the pair it is
        // derived from rather than separately accumulated.
        EXPECT_EQ(r.avgNs(), r.totalNs / r.count) << r.name;
        EXPECT_LE(r.maxNs, r.totalNs) << r.name << ": one slice cannot exceed the sum";
    }
    // Sorted by cost, so the answer to "where did the time go" is the first row.
    for (size_t i = 1; i < s.rows.size(); i++) {
        EXPECT_GE(s.rows[i - 1].totalNs, s.rows[i].totalNs) << "rows must be cost-ordered";
    }
}

// 14.4.2 — DEVICE work only. Host frames live on cajeta.thread.* tracks, and
// summing them in beside the kernels is the obvious wrong answer: they are
// wall-clock spans containing the kernels, so they would dominate the table
// and double-count the very time it is reporting.
TEST(ProfileSummary, hostFramesAreNotSummedInWithKernels) {
    if (!exists(amdgpuTrace())) GTEST_SKIP() << "fixture unavailable";
    Summary dev, host;
    std::string err;
    SummaryOptions hostOpts;
    hostOpts.host = true;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), SummaryOptions{}, &dev, &err)) << err;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), hostOpts, &host, &err)) << err;

    ASSERT_FALSE(dev.rows.empty());
    ASSERT_FALSE(host.rows.empty()) << "the fixture has host lanes too";
    // Disjoint: no name appears in both views. A reader asking for kernels must
    // not be shown a stdlib frame that happens to bracket one.
    for (const auto& d : dev.rows) {
        EXPECT_EQ(row(host, d.name), nullptr)
            << d.name << " appears in BOTH the device and host views";
    }
    // And the host view really is the other set, not an empty one that would
    // make the check above vacuous.
    EXPECT_GT(host.rows.size(), 0u);
}

// A CPU-only profile is not an error and not an empty table — it is a run that
// never touched a device, and the caller must be able to tell that apart from
// a window that excluded everything.
TEST(ProfileSummary, aRunWithNoDeviceWorkSaysSoRatherThanReportingZero) {
    if (!exists(hostOnlyTrace())) GTEST_SKIP() << "fixture unavailable";
    Summary s;
    std::string err;
    ASSERT_TRUE(cajeta::prof::summarize(hostOnlyTrace(), SummaryOptions{}, &s, &err)) << err;
    EXPECT_FALSE(s.sawAnyTrack) << "the tour fixture is CPU-only";
    EXPECT_TRUE(s.rows.empty());
    // The same trace DOES have host work, which is what makes the distinction
    // meaningful rather than "this file is empty".
    Summary h;
    SummaryOptions hostOpts;
    hostOpts.host = true;
    ASSERT_TRUE(cajeta::prof::summarize(hostOnlyTrace(), hostOpts, &h, &err)) << err;
    EXPECT_TRUE(h.sawAnyTrack);
    EXPECT_FALSE(h.rows.empty());
}

// 14.4.3 — the window is RELATIVE to the first slice. Absolute timestamps are
// host-clock nanoseconds and differ every run, so an absolute window could not
// be reused across two runs of the same program.
TEST(ProfileSummary, theWindowIsRelativeToTheFirstSlice) {
    if (!exists(amdgpuTrace())) GTEST_SKIP() << "fixture unavailable";
    Summary all;
    std::string err;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), SummaryOptions{}, &all, &err)) << err;
    ASSERT_GT(all.sliceCount, 0);

    // From 0 with no end bound is the whole run — the window's identity case.
    SummaryOptions whole;
    whole.fromNs = 0;
    whole.toNs = -1;
    Summary same;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), whole, &same, &err)) << err;
    EXPECT_EQ(same.sliceCount, all.sliceCount);

    // A window starting at 0 must therefore include the FIRST slice, which is
    // the whole point of anchoring: with absolute timestamps, 0 would be before
    // the run began and select nothing.
    SummaryOptions head;
    head.fromNs = 0;
    head.toNs = 0;
    Summary first;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), head, &first, &err)) << err;
    EXPECT_GT(first.sliceCount, 0)
        << "a zero-width window at the origin must still catch the first slice";
    EXPECT_LT(first.sliceCount, all.sliceCount) << "and must not catch them all";
}

// A window past the end selects nothing, and that is reported as tracks-present
// -but-no-slices rather than as no-device-work.
TEST(ProfileSummary, aWindowPastTheEndIsDistinctFromNoDeviceWork) {
    if (!exists(amdgpuTrace())) GTEST_SKIP() << "fixture unavailable";
    SummaryOptions far;
    far.fromNs = 3600LL * 1000000000LL;   // an hour in
    Summary s;
    std::string err;
    ASSERT_TRUE(cajeta::prof::summarize(amdgpuTrace(), far, &s, &err)) << err;
    EXPECT_TRUE(s.rows.empty());
    EXPECT_EQ(s.sliceCount, 0);
    // The distinction the CLI reports on: the device tracks are still THERE.
    EXPECT_TRUE(s.sawAnyTrack);
    EXPECT_GT(s.trackCount, 0);
}

TEST(ProfileSummary, aMissingFileIsAnErrorNotAnEmptySummary) {
    Summary s;
    std::string err;
    EXPECT_FALSE(cajeta::prof::summarize("/nonexistent/nope.pftrace",
                                         SummaryOptions{}, &s, &err));
    EXPECT_FALSE(err.empty()) << "the failure must say what went wrong";
}
