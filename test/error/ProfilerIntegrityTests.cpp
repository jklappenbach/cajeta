// cajeta-profiler Unit 9 — integrity, tier demotion, teardown (plan 9.1.e/f/g,
// 9.2.b/c; spec §10.2, §10.3, §10.4, §11.1, §11.2).
//
// The spec's own framing for this section: "nearly every failure mode found in
// the research returns success and plausible numbers." So none of these are
// tested by asking whether a call succeeded — each one drives the mechanism to
// the state that would otherwise be silent and checks that it SAYS so.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
using cajeta_test::CajetaJit;

namespace {

using UndoFn = void (*)(void*);

struct Integrity {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*tier_reset)(int32_t) = nullptr;
    int32_t (*tier)(int32_t) = nullptr;
    int32_t (*tier_reason)(int32_t) = nullptr;
    int32_t (*tier_demote)(int32_t, int32_t) = nullptr;
    int32_t (*verify)(int32_t, const int64_t*, const int64_t*, int32_t) = nullptr;
    int32_t (*set_record_threshold)(int32_t) = nullptr;
    int32_t (*note_launch)(int32_t) = nullptr;
    int32_t (*note_records)(int32_t, int64_t) = nullptr;
    int32_t (*undo_push)(int32_t, UndoFn, void*) = nullptr;
    int32_t (*undo_depth)(int32_t) = nullptr;
    int32_t (*undo_unwind)(int32_t) = nullptr;
    int32_t (*undo_commit)(int32_t) = nullptr;
    int32_t (*probe_node)(const char*) = nullptr;
    const char* (*node_advice)(int32_t, const char*) = nullptr;
};

Integrity& itg() {
    static Integrity i = [] {
        Integrity x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class I2 {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.I2");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.tier_reset = reinterpret_cast<decltype(x.tier_reset)>(sym("__cajeta_prof_tier_reset"));
        x.tier = reinterpret_cast<decltype(x.tier)>(sym("__cajeta_prof_tier"));
        x.tier_reason = reinterpret_cast<decltype(x.tier_reason)>(sym("__cajeta_prof_tier_reason"));
        x.tier_demote = reinterpret_cast<decltype(x.tier_demote)>(sym("__cajeta_prof_tier_demote"));
        x.verify = reinterpret_cast<decltype(x.verify)>(sym("__cajeta_prof_tier_verify"));
        x.set_record_threshold = reinterpret_cast<decltype(x.set_record_threshold)>(sym("__cajeta_prof_tier_set_record_threshold"));
        x.note_launch = reinterpret_cast<decltype(x.note_launch)>(sym("__cajeta_prof_tier_note_launch"));
        x.note_records = reinterpret_cast<decltype(x.note_records)>(sym("__cajeta_prof_tier_note_records"));
        x.undo_push = reinterpret_cast<decltype(x.undo_push)>(sym("__cajeta_prof_undo_push"));
        x.undo_depth = reinterpret_cast<decltype(x.undo_depth)>(sym("__cajeta_prof_undo_depth"));
        x.undo_unwind = reinterpret_cast<decltype(x.undo_unwind)>(sym("__cajeta_prof_undo_unwind"));
        x.undo_commit = reinterpret_cast<decltype(x.undo_commit)>(sym("__cajeta_prof_undo_commit"));
        x.probe_node = reinterpret_cast<decltype(x.probe_node)>(sym("__cajeta_prof_probe_node"));
        x.node_advice = reinterpret_cast<decltype(x.node_advice)>(sym("__cajeta_prof_node_advice"));
        return x;
    }();
    return i;
}

constexpr int32_t D = CAJETA_CLOCK_DOMAIN_SYNTH;

// Scratch under $TMPDIR, never /tmp.
std::string scratch(const char* leaf) {
    const char* base = getenv("TMPDIR");
    std::string dir = (base && *base) ? base : ".";
    return dir + "/cajeta_integrity_" + leaf;
}

} // namespace

// 9.1.f / §11.1 — the happy path, so the checks below are known to be capable
// of passing. A verifier that failed everything would satisfy every other test
// in this file.
TEST(ProfilerIntegrity, startupVerificationAcceptsSaneDispatchTimestamps) {
    ASSERT_NE(itg().tier_reset, nullptr) << "__cajeta_prof_tier_reset not linked";
    ASSERT_EQ(itg().tier_reset(D), 1);
    const int64_t starts[] = {1000, 2000, 3000};
    const int64_t ends[]   = {1500, 2600, 3700};
    EXPECT_EQ(itg().verify(D, starts, ends, 3), 1);
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_DEVICE);
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_NONE);
}

// §11.1 — "verifies that end exceeds start".
TEST(ProfilerIntegrity, startupVerificationDemotesWhenEndPrecedesStart) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    const int64_t starts[] = {1000, 2000};
    const int64_t ends[]   = {1500, 1900};
    EXPECT_EQ(itg().verify(D, starts, ends, 2), 0);
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_EVENT) << "a failing tier must be demoted";
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_STARTUP_CHECK);
}

// §11.1 — "that consecutive dispatches produce different timestamps". A stuck
// counter is the failure this catches, and it is invisible to every other
// check: each span on its own is perfectly well formed.
TEST(ProfilerIntegrity, startupVerificationDemotesOnAStuckDeviceCounter) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    const int64_t starts[] = {5000, 5000, 5000};
    const int64_t ends[]   = {5400, 5400, 5400};
    EXPECT_EQ(itg().verify(D, starts, ends, 3), 0);
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_STARTUP_CHECK);
}

// §11.1 — "that durations are within a sane bound".
TEST(ProfilerIntegrity, startupVerificationDemotesOnAnInsaneDuration) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    const int64_t starts[] = {1000, 2000};
    const int64_t ends[]   = {1500, 2000 + CAJETA_SPAN_MAX_NS + 1};
    EXPECT_EQ(itg().verify(D, starts, ends, 2), 0);
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_STARTUP_CHECK);
}

// 9.1.e / §11.2 — a backend that accepts every call and delivers nothing is
// the quietest failure in the whole system: the trace simply has no GPU work
// in it, which is indistinguishable from a program that launched none.
TEST(ProfilerIntegrity, backendProducingNoRecordsIsDemotedAfterTheThreshold) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    ASSERT_EQ(itg().set_record_threshold(4), 1);

    for (int i = 0; i < 3; ++i) itg().note_launch(D);
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_DEVICE) << "demoted before the threshold";

    itg().note_launch(D);                       // the fourth silent launch
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_EVENT);
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_NO_RECORDS);
}

TEST(ProfilerIntegrity, aBackendThatDeliversRecordsIsNotDemoted) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    ASSERT_EQ(itg().set_record_threshold(4), 1);
    for (int i = 0; i < 20; ++i) {
        itg().note_launch(D);
        itg().note_records(D, 1);
    }
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_DEVICE);
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_NONE);
}

// §10.4 — a documented ladder, walked one rung at a time. HOST is the floor:
// there is always a host submit-to-complete window, so there is always
// something honest left to report and never a reason to fail the run.
TEST(ProfilerIntegrity, demotionWalksTheLadderOneRungAtATimeAndFloorsAtHost) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_DEVICE);
    EXPECT_EQ(itg().tier_demote(D, CAJETA_DEMOTE_NO_CLOCK), CAJETA_PROF_TIER_EVENT);
    EXPECT_EQ(itg().tier_demote(D, CAJETA_DEMOTE_NO_CLOCK), CAJETA_PROF_TIER_HOST);
    EXPECT_EQ(itg().tier_demote(D, CAJETA_DEMOTE_NO_CLOCK), CAJETA_PROF_TIER_HOST);
    EXPECT_EQ(itg().tier(D), CAJETA_PROF_TIER_HOST);
    // The FIRST reason is the one that matters — it is why the tier fell, and
    // later reasons are consequences of already being degraded.
    EXPECT_EQ(itg().tier_reason(D), CAJETA_DEMOTE_NO_CLOCK);
}

// 9.2.c / §10.3 — a facility that fails partway through setup must leave
// nothing enabled. Reverse order is the whole point: step 3 may depend on what
// step 1 established, so unwinding forwards tears down the ground step 3 is
// standing on.
TEST(ProfilerIntegrity, partialInitializationUnwindsInReverseOrder) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    static std::vector<int>* order = nullptr;
    std::vector<int> seen;
    order = &seen;

    struct Step {
        static void one(void*)   { order->push_back(1); }
        static void two(void*)   { order->push_back(2); }
        static void three(void*) { order->push_back(3); }
    };

    EXPECT_EQ(itg().undo_push(D, &Step::one, nullptr), 1);
    EXPECT_EQ(itg().undo_push(D, &Step::two, nullptr), 2);
    EXPECT_EQ(itg().undo_push(D, &Step::three, nullptr), 3);
    EXPECT_EQ(itg().undo_depth(D), 3);

    EXPECT_EQ(itg().undo_unwind(D), 3);
    EXPECT_EQ(seen, (std::vector<int>{3, 2, 1}));
    EXPECT_EQ(itg().undo_depth(D), 0) << "the stack must be empty after unwinding";

    EXPECT_EQ(itg().undo_unwind(D), 0) << "unwinding twice must not re-run anything";
    EXPECT_EQ(seen, (std::vector<int>{3, 2, 1}));
}

// The other half: initialization that SUCCEEDS must drop the undo steps
// without running them, or a successful setup tears itself down.
TEST(ProfilerIntegrity, commitDiscardsTheUndoStackWithoutRunningIt) {
    ASSERT_EQ(itg().tier_reset(D), 1);
    static int ran = 0;
    ran = 0;
    struct Step { static void bump(void*) { ran++; } };

    itg().undo_push(D, &Step::bump, nullptr);
    itg().undo_push(D, &Step::bump, nullptr);
    EXPECT_EQ(itg().undo_depth(D), 2);
    EXPECT_EQ(itg().undo_commit(D), 2);
    EXPECT_EQ(itg().undo_depth(D), 0);
    EXPECT_EQ(ran, 0) << "commit ran the teardown it was supposed to discard";
}

// 9.1.g / §10.2 — "not installed" and "installed but you lack permission" have
// completely different fixes, and a profiler that reports the second as the
// first sends the developer to reinstall a driver they already have.
TEST(ProfilerIntegrity, missingDeviceNodeIsDistinguishedFromAnInaccessibleOne) {
    ASSERT_NE(itg().probe_node, nullptr) << "__cajeta_prof_probe_node not linked";

    const std::string absent = scratch("no_such_node");
    ::unlink(absent.c_str());
    EXPECT_EQ(itg().probe_node(absent.c_str()), CAJETA_NODE_ABSENT);

    const std::string present = scratch("readable_node");
    { FILE* f = ::fopen(present.c_str(), "w"); ASSERT_NE(f, nullptr); ::fclose(f); }
    ::chmod(present.c_str(), 0644);
    EXPECT_EQ(itg().probe_node(present.c_str()), CAJETA_NODE_OK);

    const std::string locked = scratch("locked_node");
    { FILE* f = ::fopen(locked.c_str(), "w"); ASSERT_NE(f, nullptr); ::fclose(f); }
    ::chmod(locked.c_str(), 0000);
    // Both escapes below are the same fact: mode 0000 does not always deny a
    // read, and where it does not, INACCESSIBLE is unproducible rather than
    // wrong. Skipping says so; asserting anyway would fail for a reason that
    // has nothing to do with the probe.
#ifdef _WIN32
    // Windows honours only the write bit through chmod — the file stays
    // readable at 0000, and there is no POSIX effective-uid to consult.
    // (`geteuid` is one of the calls MinGW's near-POSIX surface does not
    // carry; it is a compile error there, not a runtime skip.)
    GTEST_SKIP() << "Windows ignores POSIX read-mode bits, so a 0000 file is "
                    "still readable and the inaccessible case cannot be produced";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "running as root: mode 0000 is still readable, so the "
                        "inaccessible case cannot be produced here";
    }
    EXPECT_EQ(itg().probe_node(locked.c_str()), CAJETA_NODE_INACCESSIBLE);
#endif

    ::chmod(locked.c_str(), 0644);
    ::unlink(locked.c_str());
    ::unlink(present.c_str());
}

// §10.2 — "and the message names the fix". Distinguishing the two states is
// only useful if the developer is told what to DO about the one they have.
TEST(ProfilerIntegrity, nodeAdviceNamesTheFixAndDiffersByCause) {
    ASSERT_NE(itg().node_advice, nullptr);
    const std::string absent = itg().node_advice(CAJETA_NODE_ABSENT, "/dev/kfd");
    const std::string denied = itg().node_advice(CAJETA_NODE_INACCESSIBLE, "/dev/kfd");

    EXPECT_FALSE(absent.empty());
    EXPECT_FALSE(denied.empty());
    EXPECT_NE(absent, denied) << "the two causes must not share one message";
    // The permission case has an actionable fix and must name it.
    EXPECT_NE(denied.find("group"), std::string::npos)
        << "permission advice does not name the group fix: " << denied;
    EXPECT_EQ(std::string(itg().node_advice(CAJETA_NODE_OK, "/dev/kfd")), "");
}

// --- 9.1.a / 6.7: the device span belongs to its own launch ----------------
//
// The bracket a real span must lie in is [submit, resolution] — by CAUSALITY:
// a kernel cannot start before it was submitted, and a dispatch record cannot
// describe an execution that has not finished when the record is read. The
// bracket is NOT [submit, launch-return]: an asynchronous HIP kernel executes
// after the launch call returns by construction (6.7's reproduction measured
// dev_start landing 8.5–491 µs after the launch instant, on 144 of 144
// dispatches), and the earlier launch-return bound flagged every one of them.

namespace {
CajetaGpuEvent dispatch(int64_t hostLaunch, int64_t hostReturn,
                        int64_t devStart, int64_t devEnd,
                        int32_t tier = CAJETA_PROF_TIER_DEVICE,
                        int64_t resolved = 0) {
    CajetaGpuEvent ev{};
    ev.host_launch_ns = hostLaunch;
    ev.host_return_ns = hostReturn;
    ev.dev_start_ns = devStart;
    ev.dev_end_ns = devEnd;
    ev.tier = tier;
    ev.resolved_ns = resolved;
    return ev;
}
} // namespace

TEST(ProfilerIntegrity, deviceSpanInsideItsCausalBracketIsConsistent) {
    auto check = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        itg().jit->lookupRawSymbol("__cajeta_prof_check_dispatch"));
    ASSERT_NE(check, nullptr) << "__cajeta_prof_check_dispatch not linked";

    auto ok = dispatch(1000, 9000, 2000, 8000, CAJETA_PROF_TIER_DEVICE, 9500);
    EXPECT_EQ(check(&ok), CAJETA_SPAN_OK);

    // Touching either edge is legal — a dispatch that begins the instant it is
    // submitted is fast, not broken, and one resolved the instant it ended is
    // a synchronous collect, not a fault.
    auto flush = dispatch(1000, 9000, 1000, 9000, CAJETA_PROF_TIER_DEVICE, 9000);
    EXPECT_EQ(check(&flush), CAJETA_SPAN_OK);
}

// 6.7.1.c — the shape EVERY real async dispatch has: the kernel starts after
// the launch call already returned and completes long after it, and the
// record arrives later still. Flagging this is flagging the truth; before the
// fix this exact shape carried OUTSIDE_HOST on 144 of 144 spans of a healthy
// gfx1151 run.
TEST(ProfilerIntegrity, anAsyncSpanCompletingAfterLaunchReturnIsNotFlagged) {
    auto check = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        itg().jit->lookupRawSymbol("__cajeta_prof_check_dispatch"));
    ASSERT_NE(check, nullptr);

    const int64_t launch = 1000000000LL, ret = launch + 8000LL;  // enqueue cost
    const int64_t start = ret + 8700LL, end = start + 98000LL;   // measured shape
    auto async = dispatch(launch, ret, start, end,
                          CAJETA_PROF_TIER_DEVICE, end + 500000LL);
    EXPECT_EQ(check(&async), CAJETA_SPAN_OK)
        << "a legitimate asynchronous device span was flagged";
}

// The failure the bracket exists for: a lane converted with the WRONG clock
// domain. §6.5 measured two backends' domains 5.68 s apart, and the resulting
// span is an ordinary 6 ms of work — it is only wrong relative to the launch
// and the resolution it came from: a +5.68 s shear puts dev_end after the
// moment the record was read, and a −5.68 s shear puts dev_start before the
// submit. Both sides of the bracket are load-bearing.
TEST(ProfilerIntegrity, aDeviceSpanOutsideItsCausalBracketIsFlagged) {
    auto check = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        itg().jit->lookupRawSymbol("__cajeta_prof_check_dispatch"));
    ASSERT_NE(check, nullptr);

    const int64_t launch = 1000000000LL, ret = launch + 8000000LL;
    const int64_t resolved = ret + 2000000LL;
    const int64_t shear = 5680000000LL;                 // §6.5's measured gap
    auto sheared = dispatch(launch, ret, launch + shear,
                            launch + shear + 6000000LL,
                            CAJETA_PROF_TIER_DEVICE, resolved);
    EXPECT_TRUE(check(&sheared) & CAJETA_SPAN_OUTSIDE_HOST);
    // ... and note the span itself looks entirely reasonable.
    EXPECT_EQ(sheared.dev_end_ns - sheared.dev_start_ns, 6000000LL);

    auto early = dispatch(launch, ret, launch - 1, ret,
                          CAJETA_PROF_TIER_DEVICE, resolved);
    EXPECT_TRUE(check(&early) & CAJETA_SPAN_OUTSIDE_HOST);
    auto late = dispatch(launch, ret, launch, resolved + 1,
                         CAJETA_PROF_TIER_DEVICE, resolved);
    EXPECT_TRUE(check(&late) & CAJETA_SPAN_OUTSIDE_HOST);
}

// 6.7.1.c — TIER_DEVICE means "a vendor dispatch record supplied this span".
// An event claiming it with resolved_ns == 0 is a device claim NO record
// stands behind — 6.6.3's original reading of the amdgpu trace — and its
// timestamps have no trustworthy provenance, which is what UNCORRELATED says.
TEST(ProfilerIntegrity, aDeviceClaimWithNoRecordBehindItIsUncorrelated) {
    auto check = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        itg().jit->lookupRawSymbol("__cajeta_prof_check_dispatch"));
    ASSERT_NE(check, nullptr);

    auto orphan = dispatch(1000, 9000, 2000, 8000, CAJETA_PROF_TIER_DEVICE, 0);
    EXPECT_TRUE(check(&orphan) & CAJETA_SPAN_UNCORRELATED)
        << "a device-tier span with no resolution behind it passed as clean";

    // An unresolved non-DEVICE tier makes no record claim; the launch-return
    // fallback bracket still applies to it, nothing more.
    auto ev = dispatch(1000, 9000, 2000, 8000, CAJETA_PROF_TIER_EVENT, 0);
    EXPECT_EQ(check(&ev), CAJETA_SPAN_OK);
}

// 6.7.1.b — the memset default must be the WEAKEST claim, not the strongest.
// Every CajetaGpuEvent is minted by memset; with DEVICE = 0, an event that
// never reached a backend's begin_launch silently asserted a vendor-record
// measurement, and the viewer had to defend against it (11.1.f refuses the
// absent annotation for exactly this reason).
TEST(ProfilerIntegrity, theMemsetDefaultTierIsNotTheStrongestClaim) {
    EXPECT_NE(CAJETA_PROF_TIER_DEVICE, 0)
        << "tier 0 is the memset default and must not read as DEVICE";
    CajetaGpuEvent ev;
    memset(&ev, 0, sizeof(ev));
    EXPECT_EQ(ev.tier, CAJETA_PROF_TIER_UNKNOWN);
}

// A HOST-tier record's device span IS the host window, so containment is a
// tautology there; and a record with no device timing is absent, not
// inconsistent. Both must stay unflagged or every CPU-backend record in the
// existing seam tests starts reporting a clock fault.
TEST(ProfilerIntegrity, hostTierAndUntimedRecordsAreNotFlagged) {
    auto check = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        itg().jit->lookupRawSymbol("__cajeta_prof_check_dispatch"));
    ASSERT_NE(check, nullptr);

    auto host = dispatch(1000, 9000, 1000, 9000, CAJETA_PROF_TIER_HOST);
    EXPECT_EQ(check(&host), CAJETA_SPAN_OK);

    auto untimed = dispatch(1000, 9000, 0, 0, CAJETA_PROF_TIER_DEVICE);
    EXPECT_EQ(check(&untimed), CAJETA_SPAN_OK);
}
