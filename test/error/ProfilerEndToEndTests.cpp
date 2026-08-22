// cajeta-profiler Unit 6 — a real program produces a real trace (plan 6.1,
// spec §2, §4.3, §7.2, §8.8).
//
// The transform under test: a sampler produces periodic STACKS, Perfetto wants
// SLICES, and the bridge is a per-track diff against the previously open stack.
// Frames still present stay open; frames that vanished close innermost first;
// frames that appeared open outermost first.
//
// Every slice boundary lands on a sample TICK, not a call boundary. These tests
// assert structure and dominance, never exact durations, because exact durations
// are not a thing sampling produces — that is what §3's instrumentation tier is
// for.
#include "gtest/gtest.h"

// MinGW has no POSIX setenv/unsetenv; PortableEnv maps them onto
// _putenv_s. Without it the whole Windows release leg fails to compile
// this TU — which is exactly what happened the first time the profiler
// branch reached main and the mingw target built these files at all.
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "ProfilerTraceRead.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using cajeta_test::CajetaJit;

namespace {
struct E2E {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*arm)(void) = nullptr;
    void    (*disarm)(void) = nullptr;
    int64_t (*drain)(const char*) = nullptr;
    int64_t (*samples)(void) = nullptr;
    int64_t (*shutdown)(void) = nullptr;
    void    (*shutdownReset)(void) = nullptr;
    const char* (*outPath)(void) = nullptr;
    void    (*threadRegister)(void) = nullptr;
    void    (*threadUnregister)(void) = nullptr;
    uint64_t (*varintRead)(const uint8_t*, int32_t, int32_t*) = nullptr;
    int64_t (*frames)(void) = nullptr;
    int32_t (*hot)(int32_t) = nullptr;
};
E2E& e2e() {
    static E2E x = [] {
        E2E e;
        e.jit = CajetaJit::compile(
            "package test;\n"
            "public final class D {\n"
            "    public static int32 inner(int32 x) { return x + 1; }\n"
            "    public static int32 middle(int32 x) { return D.inner(x) + 1; }\n"
            "    public static int32 hot(int32 n) {\n"
            "        int32 acc = 0;\n"
            "        int32 i = 0;\n"
            "        while (i < n) { acc = D.middle(acc); i = i + 1; }\n"
            "        return acc;\n"
            "    }\n"
            "}\n", "test.D");
        auto s = [&](const char* n) { return e.jit->lookupRawSymbol(n); };
        e.arm     = reinterpret_cast<int32_t (*)(void)>(s("__cajeta_prof_arm"));
        e.disarm  = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_disarm"));
        e.drain   = reinterpret_cast<int64_t (*)(const char*)>(s("__cajeta_prof_drain_to_trace"));
        e.samples = reinterpret_cast<int64_t (*)(void)>(s("__cajeta_prof_sample_count"));
        e.shutdown = reinterpret_cast<int64_t (*)(void)>(s("__cajeta_prof_shutdown"));
        e.shutdownReset = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_shutdown_reset"));
        e.outPath = reinterpret_cast<const char* (*)(void)>(s("__cajeta_prof_out_path"));
        e.threadRegister = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_thread_register"));
        e.threadUnregister = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_thread_unregister"));
        e.varintRead = reinterpret_cast<uint64_t (*)(const uint8_t*, int32_t, int32_t*)>(
            s("__cajeta_pb_varint_read"));
        e.frames  = reinterpret_cast<int64_t (*)(void)>(s("__cajeta_prof_frame_count"));
        e.hot     = e.jit->lookup<int32_t (*)(int32_t)>("hot");
        return e;
    }();
    return x;
}
std::string tmpPath(const char* leaf) {
    const char* d = std::getenv("TMPDIR");
    return std::string(d ? d : "/tmp") + "/" + leaf;
}
long fileSize(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
} // namespace

TEST(ProfilerEndToEnd, drainResolves) {
    auto& e = e2e();
    ASSERT_NE(e.drain, nullptr) << "__cajeta_prof_drain_to_trace unresolved";
    ASSERT_NE(e.hot, nullptr);
}

// 6.1.a + the whole point of the unit: profile a program with a known hot
// function and get a non-trivial trace out.
TEST(ProfilerEndToEnd, profiledRunProducesATrace) {
    auto& e = e2e();
    ASSERT_NE(e.drain, nullptr);
    std::string path = tmpPath("cajeta-e2e.pftrace");
    std::remove(path.c_str());

    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    ASSERT_EQ(e.arm(), 0);
    int64_t f0 = e.frames();
    e.hot(6000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    int64_t f1 = e.frames();
    e.disarm();

    int64_t packets = e.drain(path.c_str());
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");

    EXPECT_GT(f1, f0) << "no frames captured, so the trace cannot be meaningful";
    EXPECT_GT(packets, 0) << "drain wrote no packets";
    long sz = fileSize(path);
    EXPECT_GT(sz, 0) << "trace file is empty";
    std::remove(path.c_str());
}

// Draining twice must not re-emit the first drain's samples: the ring tail
// advances. Without this a run that flushes periodically would duplicate every
// slice it had already written.
TEST(ProfilerEndToEnd, drainConsumesTheRing) {
    auto& e = e2e();
    ASSERT_NE(e.drain, nullptr);
    std::string p1 = tmpPath("cajeta-e2e-1.pftrace");
    std::string p2 = tmpPath("cajeta-e2e-2.pftrace");

    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    ASSERT_EQ(e.arm(), 0);
    e.hot(4000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    e.disarm();

    int64_t first = e.drain(p1.c_str());
    int64_t second = e.drain(p2.c_str());
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");

    EXPECT_GT(first, 0);
    // The second drain has no samples left. It may still emit nothing at all;
    // what it must NOT do is re-emit the first drain's slices.
    EXPECT_LT(second, first) << "drain did not consume the ring; samples re-emitted";
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

// An unarmed run drains to nothing rather than producing a misleading empty
// trace file that looks like a profile someone can open.
TEST(ProfilerEndToEnd, unarmedDrainWritesNothing) {
    auto& e = e2e();
    ASSERT_NE(e.drain, nullptr);
    std::string path = tmpPath("cajeta-e2e-unarmed.pftrace");
    std::remove(path.c_str());
    unsetenv("CAJETA_PROFILER");
    e.drain(path.c_str());
    // Either no file, or an empty one — but never slices from a run that was
    // never profiled.
    long sz = fileSize(path);
    EXPECT_LE(sz, 0) << "unarmed drain produced a non-empty trace";
    std::remove(path.c_str());
}

// ── 4.2.d: drain-and-flush on normal exit ─────────────────────────────────
//
// The gap this closes: until now the sampler filled the ring and the program
// exited without writing it, so §9.1's "set CAJETA_PROFILER and the run is
// profiled" produced nothing at all unless the caller knew to drain by hand.
// The compiler now emits `__cajeta_prof_arm` at main's entry and
// `__cajeta_prof_shutdown` before its return; these tests cover the runtime
// half, and `samples/tour` (6.3.a) covers the emitted half end to end.
TEST(ProfilerEndToEnd, shutdownDrainsToTheConfiguredPathWithoutAnExplicitDrain) {
    auto& e = e2e();
    ASSERT_NE(e.shutdown, nullptr) << "__cajeta_prof_shutdown unresolved";
    ASSERT_NE(e.outPath, nullptr);
    std::string path = tmpPath("cajeta-e2e-shutdown.pftrace");
    std::remove(path.c_str());

    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    setenv("CAJETA_PROFILER_OUT", path.c_str(), 1);
    e.shutdownReset();
    ASSERT_EQ(e.arm(), 0);
    EXPECT_STREQ(e.outPath(), path.c_str())
        << "the configured output path did not reach the shutdown drain";
    e.hot(6000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // No disarm and no drain call: shutdown is the whole exit path.
    int64_t packets = e.shutdown();
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");
    unsetenv("CAJETA_PROFILER_OUT");

    EXPECT_GT(packets, 0) << "exit-time shutdown wrote no packets";
    EXPECT_GT(fileSize(path), 0) << "no trace at the configured path";
    std::remove(path.c_str());
}

// Shutdown is reached from main's epilogue AND from System.exit, and both can
// run in one program. The second must not truncate the file the first wrote —
// which is what a non-idempotent implementation does, silently, leaving a
// zero-byte trace exactly where a complete one had been.
TEST(ProfilerEndToEnd, shutdownIsIdempotentAndDoesNotTruncateTheFirstTrace) {
    auto& e = e2e();
    ASSERT_NE(e.shutdown, nullptr);
    std::string path = tmpPath("cajeta-e2e-shutdown-twice.pftrace");
    std::remove(path.c_str());

    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    setenv("CAJETA_PROFILER_OUT", path.c_str(), 1);
    e.shutdownReset();
    ASSERT_EQ(e.arm(), 0);
    e.hot(4000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    int64_t first = e.shutdown();
    long sizeAfterFirst = fileSize(path);
    int64_t second = e.shutdown();
    long sizeAfterSecond = fileSize(path);
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");
    unsetenv("CAJETA_PROFILER_OUT");

    EXPECT_GT(first, 0);
    EXPECT_EQ(second, 0) << "the second shutdown re-ran the drain";
    EXPECT_GT(sizeAfterFirst, 0);
    EXPECT_EQ(sizeAfterSecond, sizeAfterFirst)
        << "a second shutdown reopened the trace and truncated it";
    std::remove(path.c_str());
}

// An unarmed program reaches main's epilogue too. Shutdown must be silent
// there — no file, no thread join, nothing — because every unprofiled binary
// now runs this code on the way out.
TEST(ProfilerEndToEnd, shutdownOnAnUnprofiledRunWritesNothing) {
    auto& e = e2e();
    ASSERT_NE(e.shutdown, nullptr);
    std::string path = tmpPath("cajeta-e2e-shutdown-unarmed.pftrace");
    std::remove(path.c_str());
    unsetenv("CAJETA_PROFILER");
    setenv("CAJETA_PROFILER_OUT", path.c_str(), 1);
    e.shutdownReset();
    ASSERT_EQ(e.arm(), 0) << "arm with CAJETA_PROFILER unset must be a quiet no-op";
    EXPECT_EQ(e.shutdown(), 0);
    unsetenv("CAJETA_PROFILER_OUT");
    EXPECT_LE(fileSize(path), 0) << "an unprofiled run left a trace file behind";
    std::remove(path.c_str());
}

// ── 6.1.b: host threads appear as distinct tracks, past two ───────────────
//
// The earlier version of this claim was carried by a one-thread/one-fiber
// assertion, which proves the track keying works and nothing about whether it
// scales. Four threads running the same hot function must land on four tracks:
// a transform keyed on anything coarser than the thread handle — the frame
// descriptor, say — collapses them into one and still emits a loadable trace.
TEST(ProfilerEndToEnd, severalHostThreadsLandOnSeveralTracks) {
    auto& e = e2e();
    ASSERT_NE(e.drain, nullptr);
    std::string path = tmpPath("cajeta-e2e-threads.pftrace");
    std::remove(path.c_str());

    ASSERT_NE(e.threadRegister, nullptr);
    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "4000", 1);
    e.shutdownReset();
    ASSERT_EQ(e.arm(), 0);
    constexpr int kThreads = 4;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; i++)
        ts.emplace_back([&] {
            // The sampler walks the REGISTRY, not every OS thread — a raw
            // std::thread is invisible to it. Registering here is what a
            // runtime-spawned carrier does for itself (cajeta_rt_core.c:714).
            e.threadRegister();
            e.hot(6000000);
            e.threadUnregister();
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    for (auto& t : ts) t.join();
    e.disarm();
    int64_t packets = e.drain(path.c_str());
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");

    ASSERT_GT(packets, 0) << "drain wrote nothing";
    int threadTracks = 0;
    for (const auto& t : cajeta_test_prof::readTracks(path.c_str(), e.varintRead))
        if (t.name.rfind("cajeta.thread.", 0) == 0) threadTracks++;
    EXPECT_GE(threadTracks, 3)
        << "samples from " << kThreads << " registered threads produced only "
        << threadTracks << " thread tracks — the transform is keying on "
           "something coarser than the thread handle";
    std::remove(path.c_str());
}

// ── 6.4: fibers are sampled at all ────────────────────────────────────────
//
// They were not. `__cajeta_prof_stack_snapshot` cast a fiber handle straight to
// `CajetaShadowStack*` on the strength of a comment claiming the shadow stack
// was the first member of `struct cajeta_fiber`. It is not — it sits behind a
// `ucontext_t` and a dozen pointers — so the read landed ~8 KB into the struct,
// came back as a non-positive depth, and every fiber sampled as "idle, no
// frames". The fiber lane was absent from every profile ever produced, and
// nothing anywhere reported a problem.
//
// Why no existing test caught it: the two callers of the snapshot in
// ProfilerThreadRegistryTests both pass THREAD handles, which genuinely are
// shadow stacks. The fiber path had exactly one caller — the sampler — and no
// test asserted what came out of it.
namespace {
struct FiberE2E {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*arm)(void) = nullptr;
    void    (*disarm)(void) = nullptr;
    int64_t (*drain)(const char*) = nullptr;
    void    (*shutdownReset)(void) = nullptr;
    uint64_t (*varintRead)(const uint8_t*, int32_t, int32_t*) = nullptr;
    int32_t (*run)(void) = nullptr;
};
FiberE2E& fiberE2e() {
    static FiberE2E x = [] {
        FiberE2E e;
        // Two fibers, each spinning for tens of milliseconds — hundreds of ticks
        // at the rate below. Short-lived workers would prove nothing: their
        // absence from a profile is just sampling.
        e.jit = CajetaJit::compile(
            "package test;\n"
            "public final class F {\n"
            "    public static int32 spin(int32 n) {\n"
            "        int32 acc = 0;\n"
            "        int32 i = 0;\n"
            "        while (i < n) { acc = acc + (i % 7); i = i + 1; }\n"
            "        return acc;\n"
            "    }\n"
            "    public static async int32 worker(int32 n) { return F.spin(n); }\n"
            "    public static int32 run() {\n"
            "        int32 total = 0;\n"
            "        scope {\n"
            "            Task<int32> a = spawn worker(6000000);\n"
            "            Task<int32> b = spawn worker(6000000);\n"
            "            total = (await a) + (await b);\n"
            "        }\n"
            "        return total;\n"
            "    }\n"
            "}\n", "test.F");
        auto s = [&](const char* n) { return e.jit->lookupRawSymbol(n); };
        e.arm    = reinterpret_cast<int32_t (*)(void)>(s("__cajeta_prof_arm"));
        e.disarm = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_disarm"));
        e.drain  = reinterpret_cast<int64_t (*)(const char*)>(s("__cajeta_prof_drain_to_trace"));
        e.shutdownReset = reinterpret_cast<void (*)(void)>(s("__cajeta_prof_shutdown_reset"));
        e.varintRead = reinterpret_cast<uint64_t (*)(const uint8_t*, int32_t, int32_t*)>(
            s("__cajeta_pb_varint_read"));
        e.run = e.jit->lookup<int32_t (*)()>("run");
        return e;
    }();
    return x;
}
} // namespace

TEST(ProfilerEndToEnd, longLivedFibersAreSampledOntoTheirOwnTracks) {
    auto& e = fiberE2e();
    ASSERT_NE(e.run, nullptr) << "run() unresolved";
    ASSERT_NE(e.drain, nullptr);
    std::string path = tmpPath("cajeta-e2e-fibers.pftrace");
    std::remove(path.c_str());

    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "4000", 1);
    e.shutdownReset();
    ASSERT_EQ(e.arm(), 0);
    ASSERT_GT(e.run(), 0) << "the spawned work did not run";
    e.disarm();
    int64_t packets = e.drain(path.c_str());
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");

    ASSERT_GT(packets, 0) << "drain wrote nothing";
    int fiberTracks = 0, threadTracks = 0;
    for (const auto& t : cajeta_test_prof::readTracks(path.c_str(), e.varintRead)) {
        if (t.name.rfind("cajeta.fiber.", 0) == 0) fiberTracks++;
        else if (t.name.rfind("cajeta.thread.", 0) == 0) threadTracks++;
    }
    std::remove(path.c_str());

    EXPECT_GE(threadTracks, 1) << "the program thread was not sampled either";
    // The assertion that was missing. Before the fix this was 0, for every
    // profile of every program that has ever used a fiber.
    EXPECT_GE(fiberTracks, 1)
        << "two fibers spun for tens of milliseconds and produced no fiber track — "
           "the sampler is reading the wrong offset behind a fiber handle";
}
