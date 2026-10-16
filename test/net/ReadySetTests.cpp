//
// ReadySetTests — request-dispatch plan Unit 1 (spec §3.4, §3.1).
//
// Today a fiber parks on ONE fd (`__cajeta_io_wait`, EPOLLONESHOT). A framer
// must wait on all of its sockets at once, so the park engine gains a
// readiness SET: a fiber adds fds with interest to a set it owns, parks on
// the set, and wakes with the members that became ready. These tests drive
// the native engine with real fibers (`__cajeta_task_run`) over AF_UNIX
// socket pairs, the FiberArenaIsolation discipline; no JIT.
//
// Plan items:
//   1.1.1 one of 1,000 members readable wakes the fiber once, with that fd
//   1.1.2 a removed member does not wake; re-added, its bytes are read
//   1.1.3 two sets, disjoint members, never wake each other; a shared
//         member wakes both
//   1.1.4 a timed wait returns 0 on expiry, and the carrier ran a sibling
//   1.1.5 live member count returns to baseline after a wave
//   1.1.6 Carriers.count() follows CAJETA_CARRIERS
//
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <sys/resource.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

extern "C" {
    void  __cajeta_task_run(void* arg, void (*trampoline)(void*),
                            void** fiber_slot);

    void*   __cajeta_io_set_create(void);
    void    __cajeta_io_set_destroy(void* set);
    int32_t __cajeta_io_set_add(void* set, int32_t fd, int32_t events);
    int32_t __cajeta_io_set_remove(void* set, int32_t fd);
    int32_t __cajeta_io_set_wait(void* set, int32_t* out_fds,
                                 int32_t* out_events, int32_t cap,
                                 int32_t timeout_ms);
    int32_t __cajeta_io_set_size(void* set);
    int64_t __cajeta_io_set_live_members(void);
    int32_t __cajeta_carriers_count(void);
    int32_t __cajeta_carriers_rule(void);
}

namespace {

constexpr int32_t IO_READ = 1;

#if !defined(_WIN32)

struct Pair { int a; int b; };

// Raise the soft fd limit to the hard one so 1,000 pairs fit.
void raiseFdLimit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

bool makePairs(std::vector<Pair>& out, int n) {
    out.clear();
    for (int i = 0; i < n; ++i) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
        out.push_back({sv[0], sv[1]});
    }
    return true;
}

void closePairs(std::vector<Pair>& v) {
    for (auto& p : v) { close(p.a); close(p.b); }
    v.clear();
}

bool waitFor(const std::atomic<int>& flag, int want, int ms = 10000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(ms);
    while (flag.load() < want && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load() >= want;
}

// One fiber: build a set over `pairs` (the `a` ends), wait, record the result.
struct SetFiber {
    std::vector<Pair>* pairs = nullptr;
    int32_t timeoutMs = -1;
    int32_t thenTimeoutMs = 0;          // timeout for waits after the first; 0 = timeoutMs
    int waits = 1;                      // how many waits to perform
    int removeFd = -1;                  // remove this member before waiting
    int readdFd = -1;                   // re-add it before the second wait
    int consumeBytes = 0;               // read this many bytes from a woken fd
    std::atomic<int> stage{0};          // 1 = set built, 2 = first wait done, ...
    std::atomic<int> done{0};
    // Per-wait results, indexed by wait number: the fiber moves straight on to
    // its next wait, so a single "last" slot races with the test thread.
    std::vector<int32_t> ns = std::vector<int32_t>(4, -99);
    std::vector<std::vector<int32_t>> fdsOf = std::vector<std::vector<int32_t>>(4);
    void* set = nullptr;
    int64_t membersAfterBuild = -1;
    int32_t n(int w) const { return ns[(size_t) w]; }
    int32_t fd(int w) const { return fdsOf[(size_t) w].empty() ? -1 : fdsOf[(size_t) w][0]; }
};

void setFiberBody(void* argp) {
    auto* s = static_cast<SetFiber*>(argp);
    s->set = __cajeta_io_set_create();
    for (auto& p : *s->pairs) {
        __cajeta_io_set_add(s->set, p.a, IO_READ);
    }
    if (s->removeFd >= 0) __cajeta_io_set_remove(s->set, s->removeFd);
    s->membersAfterBuild = __cajeta_io_set_live_members();
    s->stage.store(1);
    int32_t fds[64];
    int32_t evs[64];
    for (int w = 0; w < s->waits; ++w) {
        if (w == 1 && s->readdFd >= 0) __cajeta_io_set_add(s->set, s->readdFd, IO_READ);
        int32_t to = (w > 0 && s->thenTimeoutMs > 0) ? s->thenTimeoutMs : s->timeoutMs;
        int32_t n = __cajeta_io_set_wait(s->set, fds, evs, 64, to);
        s->ns[(size_t) w] = n;
        for (int32_t i = 0; i < n; ++i) s->fdsOf[(size_t) w].push_back(fds[i]);
        if (n > 0 && s->consumeBytes > 0) {
            // Drain inside the fiber, before the next wait re-arms the
            // member: a level-ready byte left behind is reported again.
            // Non-blocking, since a shared member's byte may already be gone.
            char buf[256];
            for (int32_t i = 0; i < n; ++i) {
                (void) !recv(fds[i], buf, (size_t) s->consumeBytes, MSG_DONTWAIT);
            }
        }
        s->stage.store(2 + w);
    }
    s->done.store(1);
}

void destroySet(SetFiber& s) {
    if (s.set) { __cajeta_io_set_destroy(s.set); s.set = nullptr; }
}

#endif  // !_WIN32

}  // namespace

#if defined(_WIN32)
TEST(ReadySetTests, linuxOnlyForNow) {
    GTEST_SKIP() << "the readiness set lands on the epoll engine first";
}
#else

// 1.1.1 — one readable member among 1,000 wakes the fiber exactly once,
// and the wake names that member and nothing else.
TEST(ReadySetTests, oneOfAThousandMembersWakesOnceWithThatFd) {
    raiseFdLimit();
    std::vector<Pair> pairs;
    ASSERT_TRUE(makePairs(pairs, 1000)) << "socketpair failed: " << strerror(errno);

    SetFiber s;
    s.pairs = &pairs;
    s.waits = 2;            // second wait: nothing else is ready → times out
    s.timeoutMs = 200;
    s.consumeBytes = 1;     // drain the byte so the second wait is quiet
    void* slot = nullptr;
    __cajeta_task_run(&s, setFiberBody, &slot);
    ASSERT_TRUE(waitFor(s.stage, 1)) << "fiber never built its set";
    EXPECT_EQ(__cajeta_io_set_size(s.set), 1000);

    const int target = 617;
    char byte = 'x';
    ASSERT_EQ(1, (int) write(pairs[target].b, &byte, 1));
    ASSERT_TRUE(waitFor(s.stage, 2)) << "fiber never woke from its first wait";
    ASSERT_EQ(s.n(0), 1) << "the first wait must report exactly one member";
    EXPECT_EQ(s.fd(0), pairs[target].a);

    ASSERT_TRUE(waitFor(s.done, 1)) << "second wait never returned";
    EXPECT_EQ(s.n(1), 0) << "with the byte consumed, nothing else may wake the set";

    destroySet(s);
    closePairs(pairs);
}

// 1.1.2 — a removed member does not wake the set; after re-add it does, and
// the bytes that arrived meanwhile are still there to read.
TEST(ReadySetTests, removedMemberIsSilentUntilReadded) {
    std::vector<Pair> pairs;
    ASSERT_TRUE(makePairs(pairs, 8));

    SetFiber s;
    s.pairs = &pairs;
    s.waits = 2;
    s.timeoutMs = 150;
    s.removeFd = pairs[3].a;
    s.readdFd = pairs[3].a;
    void* slot = nullptr;
    __cajeta_task_run(&s, setFiberBody, &slot);
    ASSERT_TRUE(waitFor(s.stage, 1));
    EXPECT_EQ(__cajeta_io_set_size(s.set), 7);

    char byte = 'y';
    ASSERT_EQ(1, (int) write(pairs[3].b, &byte, 1));
    ASSERT_TRUE(waitFor(s.stage, 2));
    EXPECT_EQ(s.n(0), 0) << "a removed member must not wake the set; reported fd "
                         << s.fd(0) << " (removed " << pairs[3].a << ")";

    // Second wait: the member is re-added first, and its pending byte is
    // level-ready, so the wait returns at once with it.
    ASSERT_TRUE(waitFor(s.done, 1));
    ASSERT_EQ(s.n(1), 1);
    EXPECT_EQ(s.fd(1), pairs[3].a);
    char got = 0;
    EXPECT_EQ(1, (int) read(pairs[3].a, &got, 1));
    EXPECT_EQ(got, 'y');

    destroySet(s);
    closePairs(pairs);
}

// 1.1.3 — two fibers on two sets with disjoint members never wake each
// other; a member present in both sets wakes both.
TEST(ReadySetTests, disjointSetsAreIndependentAndSharedMembersWakeBoth) {
    std::vector<Pair> left, right, shared;
    ASSERT_TRUE(makePairs(left, 4));
    ASSERT_TRUE(makePairs(right, 4));
    ASSERT_TRUE(makePairs(shared, 1));

    // Both fibers see their own four plus the shared one (index 4).
    std::vector<Pair> lm = left;  lm.push_back(shared[0]);
    std::vector<Pair> rm = right; rm.push_back(shared[0]);

    // L never needs to expire. R's first wait must expire (nothing of its own
    // is written), and its second must not: the shared byte is written only
    // after R's first wait expired, about 300 ms after L's second wait began,
    // so a 300 ms second wait on L raced the write under sweep load (proton,
    // 2026-10-16: L.n(1) read 0 once in 32-way, 3/3 alone).
    SetFiber L, R;
    L.pairs = &lm; L.waits = 2; L.timeoutMs = 5000; L.consumeBytes = 1;
    R.pairs = &rm; R.waits = 2; R.timeoutMs = 300; R.thenTimeoutMs = 5000; R.consumeBytes = 1;
    void* sl = nullptr; void* sr = nullptr;
    __cajeta_task_run(&L, setFiberBody, &sl);
    __cajeta_task_run(&R, setFiberBody, &sr);
    ASSERT_TRUE(waitFor(L.stage, 1));
    ASSERT_TRUE(waitFor(R.stage, 1));

    // Wake only the left set.
    char byte = 'l';
    ASSERT_EQ(1, (int) write(left[2].b, &byte, 1));
    ASSERT_TRUE(waitFor(L.stage, 2));
    ASSERT_EQ(L.n(0), 1);
    EXPECT_EQ(L.fd(0), left[2].a);
    // Right's first wait must time out, not report left's member.
    ASSERT_TRUE(waitFor(R.stage, 2));
    EXPECT_EQ(R.n(0), 0) << "a member of another set woke this one";

    // The fiber drained left's byte itself, so its second wait is about the
    // shared member only. Now the shared member: both second waits return
    // with it (one of them drains the byte; the other's drain is a no-op).
    ASSERT_EQ(1, (int) write(shared[0].b, &byte, 1));
    ASSERT_TRUE(waitFor(L.done, 1));
    ASSERT_TRUE(waitFor(R.done, 1));
    ASSERT_EQ(L.n(1), 1); EXPECT_EQ(L.fd(1), shared[0].a);
    ASSERT_EQ(R.n(1), 1); EXPECT_EQ(R.fd(1), shared[0].a);

    destroySet(L); destroySet(R);
    closePairs(left); closePairs(right); closePairs(shared);
}

// 1.1.4 — a timed wait on a quiet set returns 0 on expiry, and the carrier
// ran a sibling fiber meanwhile (the no-carrier-blocks invariant).
namespace {
struct Sibling { std::atomic<int> ticks{0}; std::atomic<int> stop{0}; std::atomic<int> done{0}; };
void siblingBody(void* argp) {
    auto* s = static_cast<Sibling*>(argp);
    while (!s->stop.load()) {
        s->ticks.fetch_add(1);
        // Yield through a tiny timed sleep so the waiter can be scheduled.
        std::this_thread::yield();
        if (s->ticks.load() > 100000000) break;
    }
    s->done.store(1);
}
}  // namespace

TEST(ReadySetTests, timedWaitExpiresWhileTheCarrierRunsASibling) {
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "1");
#else
    setenv("CAJETA_CARRIERS", "1", 1);   // before the pool starts
#endif
    std::vector<Pair> pairs;
    ASSERT_TRUE(makePairs(pairs, 4));

    SetFiber s;
    s.pairs = &pairs;
    s.waits = 1;
    s.timeoutMs = 120;
    void* slot = nullptr;
    __cajeta_task_run(&s, setFiberBody, &slot);
    ASSERT_TRUE(waitFor(s.stage, 1));

    Sibling sib;
    void* slot2 = nullptr;
    __cajeta_task_run(&sib, siblingBody, &slot2);

    auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(waitFor(s.done, 1, 5000)) << "timed wait never returned";
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(s.n(0), 0) << "a quiet set's timed wait must report 0 on expiry";
    EXPECT_GE(ms + 50, 120) << "returned far before its deadline";
    EXPECT_GT(sib.ticks.load(), 0)
        << "the sibling never ran: the waiter blocked its carrier";

    sib.stop.store(1);
    ASSERT_TRUE(waitFor(sib.done, 1));
    destroySet(s);
    closePairs(pairs);
}

// 1.1.5 — the live member count returns to its baseline after a wave of
// add, wait and remove, and after destroying a set that still has members.
TEST(ReadySetTests, liveMembersReturnToBaselineAfterAWave) {
    std::vector<Pair> pairs;
    ASSERT_TRUE(makePairs(pairs, 16));
    const int64_t base = __cajeta_io_set_live_members();

    SetFiber s;
    s.pairs = &pairs;
    s.waits = 1;
    s.timeoutMs = 50;
    void* slot = nullptr;
    __cajeta_task_run(&s, setFiberBody, &slot);
    ASSERT_TRUE(waitFor(s.stage, 1));
    EXPECT_EQ(s.membersAfterBuild, base + 16);
    ASSERT_TRUE(waitFor(s.done, 1));
    EXPECT_EQ(__cajeta_io_set_live_members(), base + 16)
        << "a wait must not change membership";
    for (auto& p : pairs) EXPECT_EQ(0, __cajeta_io_set_remove(s.set, p.a));
    EXPECT_EQ(__cajeta_io_set_live_members(), base);
    EXPECT_EQ(-1, __cajeta_io_set_remove(s.set, pairs[0].a))
        << "removing a non-member reports -1";

    // Destroy with members present drops them too.
    for (auto& p : pairs) __cajeta_io_set_add(s.set, p.a, IO_READ);
    EXPECT_EQ(__cajeta_io_set_live_members(), base + 16);
    destroySet(s);
    EXPECT_EQ(__cajeta_io_set_live_members(), base);
    closePairs(pairs);
}

// 1.1.6 — the carrier count rule follows CAJETA_CARRIERS, and the count a
// process reports is resolved once. The rule is pinned through its own
// entry because a serial run of this suite has already started the pool in
// this process; the fresh-process check (CAJETA_CARRIERS=8 reports 8
// framers, spec 3.1.1) is Unit 3's.
TEST(ReadySetTests, carriersCountFollowsTheEnvironment) {
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "3");
#else
    setenv("CAJETA_CARRIERS", "3", 1);
#endif
    EXPECT_EQ(__cajeta_carriers_rule(), 3);
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "2");
#else
    setenv("CAJETA_CARRIERS", "2", 1);
#endif
    EXPECT_EQ(__cajeta_carriers_rule(), 2);
    // Resolved once: the reported count does not move with the environment.
    int32_t first = __cajeta_carriers_count();
    EXPECT_GT(first, 0);
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "1");
#else
    setenv("CAJETA_CARRIERS", "1", 1);
#endif
    EXPECT_EQ(__cajeta_carriers_count(), first);
    EXPECT_EQ(__cajeta_carriers_rule(), 1);
}

#endif  // _WIN32
