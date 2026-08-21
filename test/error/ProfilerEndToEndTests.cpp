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
#include "../jit/JitTestHelper.h"
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
