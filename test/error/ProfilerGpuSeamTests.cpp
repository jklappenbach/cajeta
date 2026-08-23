// cajeta-profiler Unit 7 — the GPU record seam and its sinks (plan 7.1).
//
// Every test here drives the SAME entry point the launch chokepoint drives —
// __cajeta_prof_gpu_launch, with the kernel body swapped for a caller-supplied
// thunk. A test that reimplemented the seam's bookkeeping would pass against a
// seam that was never wired in, which is the failure mode this unit is most
// exposed to: the dispatch path and the collection path are far apart, and only
// one of them is easy to test.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include "ProfilerTraceRead.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <set>
#include <thread>
#include <vector>
using cajeta_test::CajetaJit;

namespace {

// Mirrors the enum in cajeta_xpu_dispatch.c. The record carries the backend as
// a plain int precisely so a consumer need not drag the XPU ABI in; a test is a
// consumer, so it gets the same deal.
constexpr int32_t XPU_CPU = 3;

struct Seam {
    std::unique_ptr<CajetaJit> jit;
    void    (*launch)(const char*, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t, uint32_t, int64_t, int32_t, int32_t,
                      void (*)(void*), void*) = nullptr;
    int32_t (*sink_register)(CajetaGpuSinkFn, void*, int32_t) = nullptr;
    int32_t (*sink_unregister)(int32_t) = nullptr;
    int32_t (*sink_count)(void) = nullptr;
    int32_t (*sink_enabled)(int32_t) = nullptr;
    int64_t (*sink_dropped)(int32_t) = nullptr;
    int64_t (*sink_delivered)(int32_t) = nullptr;
    int32_t (*sink_granularity)(int32_t) = nullptr;
    int32_t (*set_queue_cap)(int32_t) = nullptr;
    int32_t (*flush)(void) = nullptr;
    int32_t (*is_armed)(void) = nullptr;
    int64_t (*records)(void) = nullptr;
    int64_t (*last_launch_id)(void) = nullptr;
    int32_t (*trace_attach)(const char*) = nullptr;
    int32_t (*trace_detach)(void) = nullptr;
    void    (*line_enter)(const CajetaFrameDesc*) = nullptr;
    void    (*line_mark)(int32_t) = nullptr;
    void    (*line_leave)(void) = nullptr;
    int64_t (*now_ns)(void) = nullptr;
};

Seam& seam() {
    static Seam s = [] {
        Seam x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class G {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.G");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.launch = reinterpret_cast<decltype(x.launch)>(sym("__cajeta_prof_gpu_launch"));
        x.sink_register = reinterpret_cast<decltype(x.sink_register)>(sym("__cajeta_prof_gpu_sink_register"));
        x.sink_unregister = reinterpret_cast<decltype(x.sink_unregister)>(sym("__cajeta_prof_gpu_sink_unregister"));
        x.sink_count = reinterpret_cast<decltype(x.sink_count)>(sym("__cajeta_prof_gpu_sink_count"));
        x.sink_enabled = reinterpret_cast<decltype(x.sink_enabled)>(sym("__cajeta_prof_gpu_sink_enabled"));
        x.sink_dropped = reinterpret_cast<decltype(x.sink_dropped)>(sym("__cajeta_prof_gpu_sink_dropped"));
        x.sink_delivered = reinterpret_cast<decltype(x.sink_delivered)>(sym("__cajeta_prof_gpu_sink_delivered"));
        x.sink_granularity = reinterpret_cast<decltype(x.sink_granularity)>(sym("__cajeta_prof_gpu_sink_granularity"));
        x.set_queue_cap = reinterpret_cast<decltype(x.set_queue_cap)>(sym("__cajeta_prof_gpu_set_queue_cap"));
        x.flush = reinterpret_cast<decltype(x.flush)>(sym("__cajeta_prof_gpu_flush"));
        x.is_armed = reinterpret_cast<decltype(x.is_armed)>(sym("__cajeta_prof_gpu_is_armed"));
        x.records = reinterpret_cast<decltype(x.records)>(sym("__cajeta_prof_gpu_records"));
        x.last_launch_id = reinterpret_cast<decltype(x.last_launch_id)>(sym("__cajeta_prof_gpu_last_launch_id"));
        x.trace_attach = reinterpret_cast<decltype(x.trace_attach)>(sym("__cajeta_prof_gpu_trace_attach"));
        x.trace_detach = reinterpret_cast<decltype(x.trace_detach)>(sym("__cajeta_prof_gpu_trace_detach"));
        x.line_enter = reinterpret_cast<decltype(x.line_enter)>(sym("__cajeta_line_enter"));
        x.line_mark = reinterpret_cast<decltype(x.line_mark)>(sym("__cajeta_line_mark"));
        x.line_leave = reinterpret_cast<decltype(x.line_leave)>(sym("__cajeta_line_leave"));
        x.now_ns = reinterpret_cast<decltype(x.now_ns)>(sym("__cajeta_currentTimeNanos"));
        return x;
    }();
    return s;
}

void noKernel(void*) {}

// One launch through the real seam, with a no-op kernel body.
void fire(const char* name = "test.K.add", int64_t queue = 0) {
    seam().launch(name, 64, 1, 1, 32, 1, 1, /*shared=*/0, queue,
                  /*deviceId=*/0, XPU_CPU, &noKernel, nullptr);
}

// A sink that keeps every record it is handed, plus the batch sizes it saw.
struct Collector {
    std::vector<CajetaGpuEvent> got;
    std::vector<int32_t>        batches;
    std::atomic<int32_t>        faults{0};
    int32_t                     failAfter = -1;   // -1 = never fault
    int32_t                     sleepMs = 0;
    bool                        mutate = false;

    static int32_t fn(const CajetaGpuEvent* recs, int32_t n, void* user) {
        auto* c = static_cast<Collector*>(user);
        if (c->sleepMs) std::this_thread::sleep_for(std::chrono::milliseconds(c->sleepMs));
        if (c->failAfter >= 0 && static_cast<int32_t>(c->got.size()) >= c->failAfter) {
            c->faults++;
            return -1;                    // "I faulted" — the only failure a C ABI carries
        }
        c->batches.push_back(n);
        for (int32_t i = 0; i < n; i++) {
            c->got.push_back(recs[i]);
            // 7.1.f: a sink that scribbles on its copy must not be able to
            // reach another sink's. Casting away const is the point of the test.
            if (c->mutate) {
                auto* w = const_cast<CajetaGpuEvent*>(&recs[i]);
                w->launch_id = -999;
                w->kernel_name = "CLOBBERED";
            }
        }
        return 0;
    }
};

// Registration is process-global, so a test that leaks a sink poisons every
// test after it. RAII, not discipline.
struct Sink {
    int32_t id;
    Sink(Collector* c, int32_t gran) { id = seam().sink_register(&Collector::fn, c, gran); }
    ~Sink() { if (id >= 0) seam().sink_unregister(id); }
};

bool fileExists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

} // namespace

TEST(ProfilerGpuSeam, entryPointsResolve) {
    auto& s = seam();
    ASSERT_NE(s.launch, nullptr)          << "__cajeta_prof_gpu_launch unresolved";
    ASSERT_NE(s.sink_register, nullptr)   << "__cajeta_prof_gpu_sink_register unresolved";
    ASSERT_NE(s.sink_unregister, nullptr) << "__cajeta_prof_gpu_sink_unregister unresolved";
    ASSERT_NE(s.flush, nullptr)           << "__cajeta_prof_gpu_flush unresolved";
    ASSERT_NE(s.trace_attach, nullptr)    << "__cajeta_prof_gpu_trace_attach unresolved";
    ASSERT_NE(s.now_ns, nullptr)          << "__cajeta_currentTimeNanos unresolved";
}

// 7.1.a — the record carries what §5.1.1 enumerates: launch id, device start
// and end, launching thread, kernel identity and geometry.
TEST(ProfilerGpuSeam, cpuDispatchEmitsOneCompleteRecord) {
    auto& s = seam();
    Collector c;
    Sink sink(&c, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sink.id, 0);

    fire("test.K.saxpy");
    ASSERT_EQ(s.flush(), 1) << "flush should have delivered to exactly one sink";

    ASSERT_EQ(c.got.size(), 1u) << "one dispatch must produce exactly one record";
    const CajetaGpuEvent& e = c.got[0];
    EXPECT_GT(e.launch_id, 0);
    EXPECT_STREQ(e.kernel_name, "test.K.saxpy");
    EXPECT_EQ(e.backend, XPU_CPU);
    EXPECT_EQ(e.device_id, 0);
    EXPECT_EQ(e.grid_x, 64);
    EXPECT_EQ(e.block_x, 32);
    EXPECT_NE(e.host_thread, nullptr) << "the launching thread identifies the host track";
    EXPECT_GT(e.dev_end_ns, 0);
    EXPECT_GE(e.dev_end_ns, e.dev_start_ns) << "a kernel cannot end before it starts";
}

// 7.1.b — correlated to the host call site (§5.1.2). The seam reads the same
// line-info shadow stack the sampler reads, so a launch and a sample agree
// about where the program was.
TEST(ProfilerGpuSeam, recordCorrelatesToTheHostCallSite) {
    auto& s = seam();
    static CajetaFrameDesc frame = { "test.G", "launchSite", "G.cajeta" };
    Collector c;
    Sink sink(&c, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sink.id, 0);

    s.line_enter(&frame);
    s.line_mark(41);
    fire();
    s.line_leave();
    s.flush();

    ASSERT_EQ(c.got.size(), 1u);
    ASSERT_NE(c.got[0].call_site, nullptr) << "no call site captured";
    EXPECT_STREQ(c.got[0].call_site->methodName, "launchSite");
    EXPECT_STREQ(c.got[0].call_site->fileName, "G.cajeta");
    EXPECT_EQ(c.got[0].call_site_line, 41)
        << "the line must be the one live at the launch, not the frame's entry";
}

// 7.1.d — unique and monotonic under concurrent dispatch. Monotonic per thread
// is the weaker claim a non-atomic counter also satisfies; uniqueness ACROSS
// threads is the one that fails when the mint races.
TEST(ProfilerGpuSeam, launchIdsAreUniqueUnderConcurrentDispatch) {
    auto& s = seam();
    Collector c;
    s.set_queue_cap(4096);              // wide enough that nothing is dropped
    Sink sink(&c, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sink.id, 0);

    constexpr int kThreads = 4, kEach = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; t++)
        ts.emplace_back([] { for (int i = 0; i < kEach; i++) fire(); });
    for (auto& t : ts) t.join();
    s.flush();

    ASSERT_EQ(c.got.size(), static_cast<size_t>(kThreads * kEach))
        << "records were lost: the queue was sized to hold every one";
    std::set<int64_t> ids;
    for (const auto& e : c.got) ids.insert(e.launch_id);
    EXPECT_EQ(ids.size(), c.got.size()) << "duplicate launch ids under concurrency";
    s.set_queue_cap(CAJETA_GPU_SINK_QUEUE);   // a widened queue would hide the drop test
}

// 7.1.e — with nothing registered, the dispatch path is unchanged: no id is
// minted, no record is built, and the seam reports itself unarmed. The kernel
// still runs, which is the half that a "just skip everything" implementation
// gets wrong.
TEST(ProfilerGpuSeam, unarmedDispatchMintsNothingAndStillRuns) {
    auto& s = seam();
    ASSERT_EQ(s.sink_count(), 0) << "a previous test leaked a sink";
    EXPECT_EQ(s.is_armed(), 0);

    int64_t before = s.last_launch_id();
    int64_t recordsBefore = s.records();
    static int ran = 0;
    ran = 0;
    s.launch("test.K.unarmed", 1, 1, 1, 1, 1, 1, 0, 0, 0, XPU_CPU,
             [](void*) { ran++; }, nullptr);

    EXPECT_EQ(ran, 1) << "unarmed must still dispatch the kernel";
    EXPECT_EQ(s.last_launch_id(), before) << "an unarmed launch minted an id";
    EXPECT_EQ(s.records(), recordsBefore) << "an unarmed launch built a record";
}

// 7.1.f — each sink receives every record, and neither can observe or mutate
// what the other receives (§5.6.2). Sink A is registered first and scribbles on
// its copy; B must see pristine records.
TEST(ProfilerGpuSeam, twoSinksEachSeeEveryRecordAndCannotReachEachOthers) {
    auto& s = seam();
    Collector a, b;
    a.mutate = true;
    Sink sa(&a, CAJETA_GPU_SINK_BATCHED);
    Sink sb(&b, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sa.id, 0);
    ASSERT_GE(sb.id, 0);

    for (int i = 0; i < 8; i++) fire("test.K.shared");
    s.flush();

    ASSERT_EQ(a.got.size(), 8u);
    ASSERT_EQ(b.got.size(), 8u) << "the second sink did not receive every record";
    for (const auto& e : b.got) {
        EXPECT_STREQ(e.kernel_name, "test.K.shared")
            << "sink A's mutation reached sink B — they share a buffer";
        EXPECT_NE(e.launch_id, -999);
    }
}

// 7.1.g — a slow sink drops rather than blocking (§5.6.4). The drop count is
// asserted because a silent drop and a correct run are indistinguishable from
// the outside; and the wall time is asserted because "it dropped" without "and
// it did not block" would also be satisfied by a seam that simply stalled.
TEST(ProfilerGpuSeam, slowSinkDropsRatherThanBlockingTheDispatchPath) {
    auto& s = seam();
    Collector slow;
    slow.sleepMs = 5;
    // BEFORE registering: the capacity is fixed when the sink's queue is
    // allocated. Setting it afterwards changes nothing and the test then passes
    // or fails on whatever capacity the previous test happened to leave behind.
    s.set_queue_cap(8);
    Sink sink(&slow, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink.id, 0);

    constexpr int kFires = 200;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFires; i++) fire();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();

    EXPECT_GT(s.sink_dropped(sink.id), 0)
        << "a 5ms sink absorbed 200 launches without dropping — delivery is not bounded";
    // 200 launches x 5ms = 1s if the dispatch path waited on the sink.
    EXPECT_LT(elapsedMs, 500)
        << "dispatch blocked on the sink: " << elapsedMs << "ms for " << kFires << " launches";
    s.set_queue_cap(CAJETA_GPU_SINK_QUEUE);
}

// 7.1.h — a faulting sink is isolated and disabled; the rest continue (§5.6.5).
TEST(ProfilerGpuSeam, faultingSinkIsDisabledAndTheOthersContinue) {
    auto& s = seam();
    Collector bad, good;
    bad.failAfter = 0;                     // faults on its very first delivery
    Sink sb(&bad, CAJETA_GPU_SINK_BATCHED);
    Sink sg(&good, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sb.id, 0);
    ASSERT_GE(sg.id, 0);

    fire();
    s.flush();
    EXPECT_EQ(s.sink_enabled(sb.id), 0) << "a sink that reported a fault stayed enabled";
    EXPECT_EQ(s.sink_enabled(sg.id), 1);

    int64_t goodBefore = s.sink_delivered(sg.id);
    int32_t faultsBefore = bad.faults.load();
    for (int i = 0; i < 4; i++) fire();
    s.flush();

    EXPECT_GT(s.sink_delivered(sg.id), goodBefore) << "the run did not continue";
    EXPECT_EQ(bad.faults.load(), faultsBefore)
        << "a disabled sink was called again — it was reported, not isolated";
}

// 7.1.i — arming is independent of trace output (§5.6.3), in both directions.
TEST(ProfilerGpuSeam, liveSinkWritesNoTraceAndATraceNeedsNoLiveSink) {
    auto& s = seam();
    const char* path = "cajeta-gpu-seam-test.pftrace";
    remove(path);

    {   // a live consumer, no trace file
        Collector c;
        Sink sink(&c, CAJETA_GPU_SINK_PER_RECORD);
        ASSERT_GE(sink.id, 0);
        fire();
        s.flush();
        EXPECT_GE(c.got.size(), 1u);
        EXPECT_FALSE(fileExists(path)) << "a live sink emitted a trace nobody asked for";
    }

    {   // a trace, no live consumer
        ASSERT_EQ(s.sink_count(), 0);
        ASSERT_EQ(s.trace_attach(path), 0) << "trace_attach failed";
        EXPECT_EQ(s.sink_count(), 1) << "the writer is a sink like any other";
        fire("test.K.written");
        s.flush();
        s.trace_detach();
        EXPECT_TRUE(fileExists(path)) << "no trace written";
    }
    remove(path);
}

// 7.1.j — the record arrives already in the host clock domain and already
// carrying its tier, so no consumer repeats the correlation or guesses at
// provenance (§5.1.7, §5.6.6).
TEST(ProfilerGpuSeam, recordArrivesHostDomainAndTierMarked) {
    auto& s = seam();
    Collector c;
    Sink sink(&c, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sink.id, 0);

    int64_t before = s.now_ns();
    fire();
    int64_t after = s.now_ns();
    s.flush();

    ASSERT_EQ(c.got.size(), 1u);
    const CajetaGpuEvent& e = c.got[0];
    // Bracketing by the SAME clock the sampler stamps its samples with is what
    // makes "host clock domain" a checkable claim rather than a comment.
    EXPECT_GE(e.dev_start_ns, before) << "device start precedes the launch";
    EXPECT_LE(e.dev_end_ns, after)    << "device end follows the return";
    EXPECT_GE(e.host_launch_ns, before);
    EXPECT_LE(e.host_return_ns, after);
    EXPECT_EQ(e.tier, CAJETA_PROF_TIER_HOST)
        << "the CPU backend measures host submit-to-complete and must say so";
}

// 7.1.k — granularity is honored PER SINK, not globally (§5.6.8, §14.12), and
// an undeclared sink gets batched.
TEST(ProfilerGpuSeam, deliveryGranularityIsHonoredPerSink) {
    auto& s = seam();
    Collector perRecord, batched, undeclared;
    Sink sp(&perRecord, CAJETA_GPU_SINK_PER_RECORD);
    Sink sb(&batched, CAJETA_GPU_SINK_BATCHED);
    Sink su(&undeclared, 999);             // nonsense value: an undeclared sink
    ASSERT_GE(sp.id, 0);
    ASSERT_GE(sb.id, 0);
    ASSERT_GE(su.id, 0);
    EXPECT_EQ(s.sink_granularity(su.id), CAJETA_GPU_SINK_BATCHED)
        << "an undeclared sink must get the cheaper default";

    for (int i = 0; i < 16; i++) fire();
    s.flush();

    ASSERT_FALSE(perRecord.batches.empty());
    for (int32_t n : perRecord.batches)
        EXPECT_EQ(n, 1) << "a per-record sink was handed a batch of " << n;
    ASSERT_EQ(batched.got.size(), 16u);
    int32_t biggest = 0;
    for (int32_t n : batched.batches) if (n > biggest) biggest = n;
    EXPECT_GT(biggest, 1)
        << "a batched sink never got more than one record per call — batching is nominal";
}

// ── 7.1.c: the device lane is a real track, not a synthetic thread ─────────
//
// This one reads the emitted BYTES, via the shared reader in
// ProfilerTraceRead.h. Asserting that the writer's uuid helper returns different
// numbers would only test arithmetic; §7.2 is a claim about what lands in the
// file, and 6.1.d already cost a round of CI proving that a table can be perfect
// while its output is malformed.
namespace {
using cajeta_test_prof::TrackDesc;

std::vector<TrackDesc> readTracks(const char* path) {
    auto& s = seam();
    return cajeta_test_prof::readTracks(
        path, reinterpret_cast<cajeta_test_prof::VarintRead>(
                  s.jit->lookupRawSymbol("__cajeta_pb_varint_read")));
}
} // namespace

TEST(ProfilerGpuSeam, deviceLaneIsATrackHierarchyDistinctFromHostThreads) {
    auto& s = seam();
    const char* path = "cajeta-gpu-tracks-test.pftrace";
    remove(path);

    Collector c;
    Sink sink(&c, CAJETA_GPU_SINK_BATCHED);   // to learn the launching thread
    ASSERT_GE(sink.id, 0);
    ASSERT_EQ(s.trace_attach(path), 0);
    fire("test.K.tracked");
    s.flush();
    s.trace_detach();
    ASSERT_EQ(c.got.size(), 1u);
    const uint64_t hostUuid = reinterpret_cast<uint64_t>(c.got[0].host_thread);

    std::vector<TrackDesc> tracks = readTracks(path);
    remove(path);
    ASSERT_GE(tracks.size(), 4u)
        << "expected host + device + context + queue tracks, got " << tracks.size();

    const TrackDesc *device = nullptr, *context = nullptr, *queue = nullptr, *host = nullptr;
    for (const auto& d : tracks) {
        if (d.uuid == hostUuid) host = &d;
        else if (d.name.find("device") != std::string::npos) device = &d;
        else if (d.name.find("context") != std::string::npos) context = &d;
        else if (d.name.find("queue") != std::string::npos) queue = &d;
    }
    ASSERT_NE(host, nullptr)    << "the launching host thread has no track";
    ASSERT_NE(device, nullptr)  << "no device track — the device lane is synthetic";
    ASSERT_NE(context, nullptr) << "no context track (spec 7.2)";
    ASSERT_NE(queue, nullptr)   << "no queue track (spec 7.2)";

    EXPECT_NE(device->uuid, hostUuid) << "the device lane reused the host thread's track";
    EXPECT_EQ(context->parent, device->uuid) << "context is not parented to its device";
    EXPECT_EQ(queue->parent, context->uuid)  << "queue is not parented to its context";
    EXPECT_EQ(device->parent, 0u)            << "the device track should be a root";
}

// ── 7.3.c: what a live sink costs, stated separately from trace writing ────
//
// §13.6 requires this precisely because the two consumers sit in different
// places: the scheduler's feedback loop is on the program's critical path and
// the trace writer is not. A single combined "profiling overhead" number would
// let the file writer's cost be charged to the consumer that cannot afford it.
//
// Reported, not asserted tightly. The bound below is deliberately loose — a
// microbenchmark on a shared CI runner cannot defend a tight one, and a
// flaky-but-strict number gets deleted, which is worse than a loose one that
// stays. What it does catch is a regression of the kind that matters: delivery
// becoming synchronous, or publication starting to allocate.
TEST(ProfilerGpuSeam, liveSinkDeliveryCostIsMeasuredAndBounded) {
    auto& s = seam();
    enum { kReps = 20000 };
    auto measure = [&](const char* what) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; i++) fire();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        double per = static_cast<double>(ns) / kReps;
        printf(" RESULT t7_ns_per_launch_%s=%.1f\n", what, per);
        return per;
    };

    double unarmed = measure("unarmed");

    double live;
    {
        Collector c;
        s.set_queue_cap(4096);
        Sink sink(&c, CAJETA_GPU_SINK_PER_RECORD);
        ASSERT_GE(sink.id, 0);
        live = measure("live_sink");
        s.flush();
    }

    double traced;
    const char* path = "cajeta-gpu-cost-test.pftrace";
    remove(path);
    {
        ASSERT_EQ(s.trace_attach(path), 0);
        traced = measure("trace_writer_sink");
        s.trace_detach();
    }
    remove(path);
    s.set_queue_cap(CAJETA_GPU_SINK_QUEUE);

    printf(" RESULT t7_live_sink_overhead_ns=%.1f\n", live - unarmed);
    printf(" RESULT t7_trace_sink_overhead_ns=%.1f\n", traced - unarmed);

    // The publish path stamps two clocks, reads one shadow frame and copies a
    // record into a ring. Sub-microsecond is a generous ceiling for that; a
    // synchronous handoff to the consumer would blow straight through it.
    EXPECT_LT(live - unarmed, 5000.0)
        << "live-sink publication costs " << (live - unarmed)
        << "ns per launch — delivery is no longer decoupled from dispatch";
}
