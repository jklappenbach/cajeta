// cajeta-profiler Unit 4 — the sampler (plan 4.1, spec §2.1/§2.2/§2.5/§2.6/
// §2.9/§9.1/§9.2/§9.4).
//
// A dedicated thread walks the Unit 3 registry on an interval and copies each
// live stack into a ring buffer. Nothing on the sampled program's path changes:
// arming starts a thread, it does not add a probe. That is what makes §2.2's
// promise possible — a binary built with default flags is profilable with no
// rebuild, because the frames the sampler reads are the line-info probes every
// build already carries.
//
// Driven through JIT-resolved symbols for the same reason as Unit 3: the state
// lives in the JIT runtime copy, which is where a profiled program runs.
#include "gtest/gtest.h"

#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv + getpid on MinGW
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
using cajeta_test::CajetaJit;

namespace {

struct Sampler {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*arm)(void) = nullptr;
    void    (*disarm)(void) = nullptr;
    int32_t (*isArmed)(void) = nullptr;
    int64_t (*samples)(void) = nullptr;
    int64_t (*drops)(void) = nullptr;
    int64_t (*frames)(void) = nullptr;
    int32_t (*intervalUs)(void) = nullptr;
    int32_t (*ringCap)(void) = nullptr;
    const char* (*outPath)(void) = nullptr;
    int32_t (*busy)(int32_t) = nullptr;
};

Sampler& sam() {
    static Sampler s = [] {
        Sampler x;
        x.jit = CajetaJit::compile(
            "package test;\n"
            "public final class D {\n"
            "    public static int32 leaf(int32 x) { return x + 1; }\n"
            "    public static int32 mid(int32 x) { return D.leaf(x) + 1; }\n"
            "    public static int32 busy(int32 n) {\n"
            "        int32 acc = 0;\n"
            "        int32 i = 0;\n"
            "        while (i < n) { acc = D.mid(acc); i = i + 1; }\n"
            "        return acc;\n"
            "    }\n"
            "}\n", "test.D");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.arm        = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_arm"));
        x.disarm     = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_disarm"));
        x.isArmed    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_is_armed"));
        x.samples    = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_sample_count"));
        x.drops      = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_drop_count"));
        x.frames     = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_frame_count"));
        x.intervalUs = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_interval_us"));
        x.ringCap    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_ring_capacity"));
        x.outPath    = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_out_path"));
        x.busy       = x.jit->lookup<int32_t (*)(int32_t)>("busy");
        return x;
    }();
    return s;
}

void clearEnv() {
    unsetenv("CAJETA_PROFILER");
    unsetenv("CAJETA_PROFILER_HZ");
    unsetenv("CAJETA_PROFILER_RING");
    unsetenv("CAJETA_PROFILER_OUT");
}

} // namespace

TEST(ProfilerSampler, entryPointsResolve) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr)        << "__cajeta_prof_arm unresolved";
    ASSERT_NE(s.disarm, nullptr)     << "__cajeta_prof_disarm unresolved";
    ASSERT_NE(s.isArmed, nullptr)    << "__cajeta_prof_is_armed unresolved";
    ASSERT_NE(s.samples, nullptr)    << "__cajeta_prof_sample_count unresolved";
    ASSERT_NE(s.drops, nullptr)      << "__cajeta_prof_drop_count unresolved";
    ASSERT_NE(s.frames, nullptr)     << "__cajeta_prof_frame_count unresolved";
    ASSERT_NE(s.intervalUs, nullptr) << "__cajeta_prof_interval_us unresolved";
    ASSERT_NE(s.ringCap, nullptr)    << "__cajeta_prof_ring_capacity unresolved";
    ASSERT_NE(s.outPath, nullptr)    << "__cajeta_prof_out_path unresolved";
}

// 4.1.a — unset means nothing starts. Asserted as "not armed and no samples
// EVER accrued", not merely "not armed": a sampler that started and then idled
// would satisfy the weaker check.
TEST(ProfilerSampler, unsetArmsNothing) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr);
    clearEnv();
    EXPECT_EQ(s.isArmed(), 0);
    int64_t before = s.samples();
    s.busy(200000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(s.samples(), before) << "samples accrued with CAJETA_PROFILER unset";
    EXPECT_EQ(s.isArmed(), 0);
}

// 4.1.b + 4.1.c — armed, samples accumulate while the program runs; and the
// program under test is the SAME default-flags JIT compile every other test in
// this file uses, which is 4.1.c: no rebuild, no special flags.
TEST(ProfilerSampler, armedAccumulatesSamples) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr);
    clearEnv();
    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    ASSERT_EQ(s.arm(), 0);
    EXPECT_EQ(s.isArmed(), 1);
    int64_t before = s.samples();
    int64_t fbefore = s.frames();
    s.busy(4000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int64_t after = s.samples();
    int64_t fafter = s.frames();
    s.disarm();
    clearEnv();
    EXPECT_GT(after, before) << "armed sampler produced no samples";
    // Load-bearing for idleProgramYieldsNoFrames, which asserts frames do NOT
    // grow: without a test proving the counter CAN grow, "no frames while idle"
    // is satisfied by a frame counter that never increments at all.
    EXPECT_GT(fafter, fbefore) << "sampler captured samples but never a frame";
    EXPECT_EQ(s.isArmed(), 0);
}

// 4.1.e — configured rate honored, documented default when unset.
TEST(ProfilerSampler, rateHonoredAndDefaulted) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr);
    clearEnv();
    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "200", 1);       // 200 Hz -> 5000 us
    ASSERT_EQ(s.arm(), 0);
    EXPECT_EQ(s.intervalUs(), 5000);
    s.disarm();

    unsetenv("CAJETA_PROFILER_HZ");
    ASSERT_EQ(s.arm(), 0);
    EXPECT_EQ(s.intervalUs(), 1000) << "default rate is 1 kHz (spec §2.6)";
    s.disarm();
    clearEnv();
}

// 4.1.f — ring overflow drops and COUNTS the drop. Nothing drains the ring in
// this unit (the writer is Unit 5), so a small ring plus a running program must
// overflow. The count is the assertion: a silent drop and a correct run are
// indistinguishable from outside, which is the same trap Unit 3's 3.1.g covered.
TEST(ProfilerSampler, ringOverflowDropsAndCounts) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr);
    clearEnv();
    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "5000", 1);
    setenv("CAJETA_PROFILER_RING", "8", 1);
    ASSERT_EQ(s.arm(), 0);
    EXPECT_EQ(s.ringCap(), 8);
    int64_t before = s.drops();
    s.busy(6000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    int64_t after = s.drops();
    s.disarm();
    clearEnv();
    EXPECT_GT(after, before) << "ring never overflowed, or drops were not counted";
}

// 4.1.g — sampling an idle program produces no spurious FRAMES. Samples may
// still be taken (the sampler ticks regardless); what must not happen is frames
// appearing from a program that is not in Cajeta code.
TEST(ProfilerSampler, idleProgramYieldsNoFrames) {
    auto& s = sam();
    ASSERT_NE(s.arm, nullptr);
    clearEnv();
    setenv("CAJETA_PROFILER", "1", 1);
    setenv("CAJETA_PROFILER_HZ", "2000", 1);
    ASSERT_EQ(s.arm(), 0);
    int64_t f0 = s.frames();
    int64_t s0 = s.samples();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // run nothing
    int64_t f1 = s.frames();
    int64_t s1 = s.samples();
    s.disarm();
    clearEnv();
    EXPECT_GT(s1, s0) << "sampler did not tick while idle";
    EXPECT_EQ(f1, f0) << "idle program produced frames";
}

// 4.1.d depends on 4.2.e (codegen must register that line-info is on, via a
// ctor — NOT a weak extern; see the unit's design note and
// cajeta_rt_core.c:692). Expected RED until that lands.
TEST(ProfilerSampler, lineInfoOffFailsLoudlyAtArm) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class E {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n", "test.E");
    auto armFn = reinterpret_cast<int32_t (*)(void)>(
        jit->lookupRawSymbol("__cajeta_prof_arm"));
    ASSERT_NE(armFn, nullptr);
    auto lineInfoOn = reinterpret_cast<int32_t (*)(void)>(
        jit->lookupRawSymbol("__cajeta_line_info_is_present"));
    ASSERT_NE(lineInfoOn, nullptr) << "__cajeta_line_info_is_present unresolved (4.2.e)";
    // Line-info is ON by default, so this compile must report present. The
    // negative half is the test below.
    EXPECT_EQ(lineInfoOn(), 1) << "default build did not register line-info presence";
}

// 4.1.d, the half that matters (spec §2.5). A profiler that armed on a
// --line-info=off binary would produce a trace that loads, renders, and is
// entirely empty of frames — indistinguishable from a program that did
// nothing. Refusing is the requirement, and refusing LOUDLY is the point:
// a silent -2 satisfies a return-code check and still leaves the developer
// staring at an empty trace wondering what they did wrong.
//
// Both halves are needed. A check with no test that it FIRES is how a
// silently-disabled check reads as green.
TEST(ProfilerSampler, lineInfoOffRefusesToArmAndSaysWhy) {
    CajetaJit::Options opts;
    opts.lineInfoEnabled = false;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public final class E2 {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n", "test.E2", opts);
    ASSERT_NE(jit, nullptr);

    auto lineInfoOn = reinterpret_cast<int32_t (*)(void)>(
        jit->lookupRawSymbol("__cajeta_line_info_is_present"));
    auto armFn = reinterpret_cast<int32_t (*)(void)>(
        jit->lookupRawSymbol("__cajeta_prof_arm"));
    auto disarmFn = reinterpret_cast<void (*)(void)>(
        jit->lookupRawSymbol("__cajeta_prof_disarm"));
    ASSERT_NE(lineInfoOn, nullptr);
    ASSERT_NE(armFn, nullptr);

    ASSERT_EQ(lineInfoOn(), 0)
        << "a --line-info=off build still registered line-info presence, so the "
           "refusal below would be testing nothing";

    // arm() is a no-op when CAJETA_PROFILER is unset (§9.1), which would make
    // this pass for the wrong reason.
    const char* prev = ::getenv("CAJETA_PROFILER");
    const std::string saved = prev ? prev : "";
    ::setenv("CAJETA_PROFILER", "1", 1);

    // Capture stderr so "loudly" is asserted rather than assumed.
    // Per-PROCESS filename. The sweep runs 32 cajeta_test processes against one
    // shared TMPDIR, so a fixed name is a shared mutable file between them —
    // and this test redirects the whole process's stderr into it, which makes
    // a collision look like "the refusal was silent" rather than like a
    // clobbered file.
    const char* base = ::getenv("TMPDIR");
    const std::string errPath =
        std::string(base && *base ? base : ".") + "/cajeta_lineinfo_refusal_"
        + std::to_string(static_cast<long long>(cajeta_getpid())) + ".txt";
    fflush(stderr);
    const int savedErr = ::dup(STDERR_FILENO);
    const int capture = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(capture, 0);
    ::dup2(capture, STDERR_FILENO);

    const int32_t rc = armFn();

    fflush(stderr);
    ::dup2(savedErr, STDERR_FILENO);
    ::close(savedErr);
    ::close(capture);

    if (prev) ::setenv("CAJETA_PROFILER", saved.c_str(), 1);
    else      ::unsetenv("CAJETA_PROFILER");
    if (rc == 0 && disarmFn) disarmFn();     // it armed anyway; do not leak the thread

    EXPECT_EQ(rc, -2) << "armed on a binary with no frames to sample (spec §2.5)";

    std::ifstream in(errPath);
    std::string said((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();
    ::remove(errPath.c_str());
    EXPECT_NE(said.find("refusing to arm"), std::string::npos)
        << "the refusal was silent; stderr held: [" << said << "]";
    EXPECT_NE(said.find("--debug-info=off"), std::string::npos)
        << "the message does not name the cause; stderr held: [" << said << "]";
    // ...and the fix, which is the half that makes it actionable. `--line-info`
    // is not a flag the CLI accepts; the message said so until 2026-08-22.
    EXPECT_NE(said.find("--debug-info=line"), std::string::npos)
        << "the message does not name the fix; stderr held: [" << said << "]";
}
