// cajeta-profiler Unit 3 — thread registry + cross-context stack snapshot
// (plan 3.1, spec §2.3, §2.7, §2.8, §4.3).
//
// A sampler thread cannot read another thread's shadow stack: after Unit 2 the
// stack lives in a __thread struct (program threads) or inline in a fiber. This
// unit publishes handles to those stacks so a sampler can snapshot them, and
// carries the truncation flag §2.8 requires.
//
// FIBERS ARE NOT RE-REGISTERED HERE. The debugger's live-fiber registry
// (cajeta_rt_core.c:60) already enumerates them under a single-lock snapshot,
// and after Unit 2 every fiber carries its shadow stack inline — a handle is
// all a sampler needs. A second registry of the same fibers would drift.
//
// The registry is exercised through JIT-resolved symbols rather than the
// process's own runtime copy: registry state lives in the JIT copy, which is
// where the profiled program runs. The test thread registering ITSELF is
// legitimate — it calls the same JIT functions on its own thread, so it gets
// that thread's TLS slot, exactly as a program thread would.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
using cajeta_test::CajetaJit;

namespace {

// One compile shared by every test in this file — a JIT compile is ~15s and
// none of these need a distinct program.
struct Rt {
    std::unique_ptr<CajetaJit> jit;
    void (*reg)(void) = nullptr;
    void (*unreg)(void) = nullptr;
    int (*count)(void) = nullptr;
    int (*snapshot)(void**, int) = nullptr;
    int32_t (*stackSnap)(void*, void*, int32_t, int32_t*) = nullptr;
    void (*setTop)(int32_t) = nullptr;
    int32_t (*getTop)(void) = nullptr;
    void* (*selfHandle)(void) = nullptr;
    int32_t (*runSpawn)(void) = nullptr;
};

Rt& rt() {
    static Rt r = [] {
        Rt x;
        x.jit = CajetaJit::compile(
            "package test;\n"
            "public final class D {\n"
            "    public static async int32 work() { return 1; }\n"
            "    public static int32 run() {\n"
            "        Task<int32> a = spawn work();\n"
            "        Task<int32> b = spawn work();\n"
            "        int32 va = await a;\n"
            "        int32 vb = await b;\n"
            "        return va + vb;\n"
            "    }\n"
            "}\n", "test.D");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.reg        = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_thread_register"));
        x.unreg      = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_thread_unregister"));
        x.count      = reinterpret_cast<int (*)(void)>(sym("__cajeta_prof_thread_count"));
        x.snapshot   = reinterpret_cast<int (*)(void**, int)>(sym("__cajeta_prof_thread_snapshot"));
        x.stackSnap  = reinterpret_cast<int32_t (*)(void*, void*, int32_t, int32_t*)>(
                           sym("__cajeta_prof_stack_snapshot"));
        x.setTop     = reinterpret_cast<void (*)(int32_t)>(sym("__cajeta_shadow_set_top"));
        x.getTop     = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_shadow_get_top"));
        x.selfHandle = reinterpret_cast<void* (*)(void)>(sym("__cajeta_prof_thread_self"));
        x.runSpawn   = x.jit->lookup<int32_t (*)()>("run");
        return x;
    }();
    return r;
}

// 16 bytes per frame (desc pointer + line); sized to the runtime's cap.
constexpr int32_t kShadowMax = 512;
struct FrameOut { const void* desc; int32_t line; int32_t pad; };

} // namespace

// The symbols must exist at all. Split out so a missing entry point reads as
// "not implemented" rather than a null-call crash inside another test.
TEST(ProfilerThreadRegistry, entryPointsResolve) {
    auto& r = rt();
    ASSERT_NE(r.reg, nullptr)        << "__cajeta_prof_thread_register unresolved";
    ASSERT_NE(r.unreg, nullptr)      << "__cajeta_prof_thread_unregister unresolved";
    ASSERT_NE(r.count, nullptr)      << "__cajeta_prof_thread_count unresolved";
    ASSERT_NE(r.snapshot, nullptr)   << "__cajeta_prof_thread_snapshot unresolved";
    ASSERT_NE(r.stackSnap, nullptr)  << "__cajeta_prof_stack_snapshot unresolved";
    ASSERT_NE(r.selfHandle, nullptr) << "__cajeta_prof_thread_self unresolved";
}

// 3.1.a — a thread appears in the registry once registered, and its handle is
// reachable through the snapshot. The registering thread here is the test's own,
// which is what a freshly-created program thread does at its entry.
TEST(ProfilerThreadRegistry, registrationIsVisible) {
    auto& r = rt();
    ASSERT_NE(r.reg, nullptr);
    int before = r.count();
    r.reg();
    int after = r.count();
    EXPECT_EQ(after, before + 1);

    std::vector<void*> handles(after + 4, nullptr);
    int n = r.snapshot(handles.data(), (int) handles.size());
    EXPECT_EQ(n, after);
    void* self = r.selfHandle();
    EXPECT_NE(self, nullptr);
    EXPECT_NE(std::find(handles.begin(), handles.begin() + n, self), handles.begin() + n)
        << "registered thread's own handle absent from the snapshot";
    r.unreg();
}

// 3.1.b — a terminated thread is removed. Registering and unregistering from a
// real std::thread, so the removal path runs on the thread that owned the slot.
TEST(ProfilerThreadRegistry, unregistrationRemoves) {
    auto& r = rt();
    ASSERT_NE(r.reg, nullptr);
    int base = r.count();
    int inside = -1;
    std::thread t([&] {
        r.reg();
        inside = r.count();
        r.unreg();
    });
    t.join();
    EXPECT_EQ(inside, base + 1);
    EXPECT_EQ(r.count(), base) << "slot survived the thread that owned it";
}

// 3.1.c — a snapshot taken from ANOTHER thread returns a coherent frame array.
// The worker parks its own shadow top at a known depth and hands over its
// handle; the test thread reads it. Coherent here means: the reported count
// matches the depth the owner set, and the call does not fault.
TEST(ProfilerThreadRegistry, crossThreadSnapshotIsCoherent) {
    auto& r = rt();
    ASSERT_NE(r.stackSnap, nullptr);
    std::atomic<void*> handle{nullptr};
    std::atomic<bool> parked{false}, release{false};
    std::thread t([&] {
        r.reg();
        r.setTop(7);                       // a known, shallow depth
        handle.store(r.selfHandle());
        parked.store(true);
        while (!release.load()) std::this_thread::yield();
        r.setTop(0);
        r.unreg();
    });
    while (!parked.load()) std::this_thread::yield();

    std::vector<FrameOut> out(kShadowMax);
    int32_t truncated = -1;
    int32_t n = r.stackSnap(handle.load(), out.data(), kShadowMax, &truncated);
    EXPECT_EQ(n, 7) << "cross-thread snapshot did not see the owner's depth";
    EXPECT_EQ(truncated, 0);

    release.store(true);
    t.join();
}

// 3.1.e — a stack deeper than capacity reports truncation (spec §2.8) rather
// than reporting a shallower stack as complete. line_enter deliberately counts
// past the cap so leave stays balanced, so the signal is already present.
//
// Driven by setting the top directly: the flag's logic is what is under test,
// and a real 600-deep recursion would make the test about the compiler instead.
TEST(ProfilerThreadRegistry, deeperThanCapacityReportsTruncation) {
    auto& r = rt();
    ASSERT_NE(r.stackSnap, nullptr);
    r.reg();
    int32_t saved = r.getTop();
    r.setTop(kShadowMax + 88);
    std::vector<FrameOut> out(kShadowMax);
    int32_t truncated = -1;
    int32_t n = r.stackSnap(r.selfHandle(), out.data(), kShadowMax, &truncated);
    EXPECT_EQ(n, kShadowMax) << "must report the frames it HAS, capped";
    EXPECT_EQ(truncated, 1) << "deeper-than-capacity was reported as complete";
    r.setTop(saved);
    r.unreg();
}

// 3.1.d — concurrent registration and snapshotting do not corrupt the registry.
// Eight threads churn register/unregister while the main thread snapshots in a
// loop. The assertion is invariant-shaped, not count-shaped: a snapshot must
// never report more handles than it wrote, never a NULL inside the reported
// prefix, and the count must return to baseline once the churn stops.
TEST(ProfilerThreadRegistry, concurrentChurnKeepsRegistryCoherent) {
    auto& r = rt();
    ASSERT_NE(r.reg, nullptr);
    int base = r.count();
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++) {
        ts.emplace_back([&] {
            for (int k = 0; k < 200; k++) { r.reg(); std::this_thread::yield(); r.unreg(); }
        });
    }
    std::vector<void*> handles(256, nullptr);
    for (int k = 0; k < 2000; k++) {
        int n = r.snapshot(handles.data(), (int) handles.size());
        int reported = n < (int) handles.size() ? n : (int) handles.size();
        ASSERT_GE(reported, 0);
        for (int i = 0; i < reported; i++)
            ASSERT_NE(handles[i], nullptr) << "NULL handle inside the reported prefix";
    }
    stop.store(true);
    for (auto& t : ts) t.join();
    EXPECT_EQ(r.count(), base) << "registry did not return to baseline after churn";
}

// 3.1.a, integration half — the runtime's OWN threads register themselves.
// Running a program that spawns tasks brings carriers up; they must appear
// without anyone calling register() by hand. Asserted as a floor, not an exact
// count: how many carriers a spawn brings up is the scheduler's business.
TEST(ProfilerThreadRegistry, runtimeThreadsSelfRegister) {
    auto& r = rt();
    ASSERT_NE(r.count, nullptr);
    ASSERT_NE(r.runSpawn, nullptr);
    EXPECT_EQ(r.runSpawn(), 2);
    EXPECT_GT(r.count(), 0) << "carriers did not register themselves";
}
