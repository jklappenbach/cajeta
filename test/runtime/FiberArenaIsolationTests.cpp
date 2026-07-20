// Frame-arena × fiber isolation (the cajeta-http `http:0.11` regression),
// pinned at the runtime's extern "C" surface where no optimizer can fold
// the witness reads away.
//
// The arena's O(1) mark/reset assumes LIFO scope nesting per LOGICAL
// stack. Fibers interleave many logical stacks on one carrier thread, so
// a per-THREAD arena let one fiber's scope-exit reset reclaim a PARKED
// fiber's live allocations: cajeta-http's client fiber parked in
// readAsync while its 8 KiB scratch lived in the arena, the server
// connection fiber's older frame exited and reset the shared bump
// pointer straight through it, and the resumed fiber found its buffer
// reclaimed (SIGABRT: array index 0 out of bounds for dimension size 0,
// ~5%/run, extremely layout-sensitive). Fixed by giving every fiber its
// own arena (cajeta_fiber.arena via the __cajeta_arena_ptr accessor —
// the scope_top/drop_top pattern); non-fiber threads keep the __thread
// arena.
//
// The test replays the interleave with two real fibers pinned to ONE
// carrier (CAJETA_CARRIERS=1, set before the pool starts) and a staged
// condvar handshake, exactly the compiler's emitted sequence:
//
//   1. fiber B takes its frame MARK at the low-water bump, then parks
//        (the server-connection fiber's early frame)
//   2. fiber A allocates a patterned 8 KiB block above it, then parks
//        (readResponse's scratch)
//   3. B resumes, RESETs to its mark, and allocates fresh zeroed blocks
//        (the frame exit + next request's frames)
//   4. A resumes and re-reads its pattern
//
// Per-thread arena: step 3 rolls straight through A's live block and the
// zeroed re-allocation wipes it — A reads garbage. Per-fiber arenas: A's
// block is untouchable by construction, at any carrier count.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
    void*    __cajeta_arena_alloc(uint64_t size);
    uint64_t __cajeta_arena_mark(void);
    void     __cajeta_arena_reset(uint64_t mark);

    void  __cajeta_task_run(void* arg, void (*trampoline)(void*),
                            void** fiber_slot);

    void* __cajeta_lock_new(void);
    void  __cajeta_lock_acquire(void* lock);
    void  __cajeta_lock_release(void* lock);
    void* __cajeta_condvar_new(void);
    void  __cajeta_condvar_wait(void* cv, void* lock);
    void  __cajeta_condvar_notify_all(void* cv);
}

namespace {

constexpr size_t kBlock = 8192;

struct Stage {
    void* lock = nullptr;
    void* cv = nullptr;
    int stage = 0;                 // guarded by lock
    std::atomic<int> doneCount{0};
    std::atomic<int> intactBytes{-1};

    void advanceTo(int s) {
        __cajeta_lock_acquire(lock);
        stage = s;
        __cajeta_condvar_notify_all(cv);
        __cajeta_lock_release(lock);
    }
    // Parks the calling fiber (condvar wait takes the fiber path) until
    // the stage reaches `s`.
    void awaitStage(int s) {
        __cajeta_lock_acquire(lock);
        while (stage < s) {
            __cajeta_condvar_wait(cv, lock);
        }
        __cajeta_lock_release(lock);
    }
};

// Fiber B — steps 1 + 3: early mark, park, reset-and-reallocate.
void fiberB(void* argp) {
    auto* st = static_cast<Stage*>(argp);
    uint64_t markB = __cajeta_arena_mark();          // low-water mark
    (void) __cajeta_arena_alloc(64);                 // B's own frame data
    st->advanceTo(1);
    st->awaitStage(2);                               // park; A allocates now
    __cajeta_arena_reset(markB);                     // the dangerous reset
    // The next frames' allocations — zeroed, like every arena alloc.
    for (int i = 0; i < 8; ++i) {
        (void) __cajeta_arena_alloc(kBlock);
    }
    st->advanceTo(3);
    st->doneCount.fetch_add(1);
}

// Fiber A — steps 2 + 4: allocate + pattern above B's mark, park, verify.
void fiberA(void* argp) {
    auto* st = static_cast<Stage*>(argp);
    st->awaitStage(1);                               // B's mark is taken
    auto* mine = static_cast<unsigned char*>(__cajeta_arena_alloc(kBlock));
    for (size_t i = 0; i < kBlock; ++i) {
        mine[i] = static_cast<unsigned char>((i % 97) + 1);   // never 0
    }
    st->advanceTo(2);
    st->awaitStage(3);                               // park across B's reset
    int intact = 0;
    for (size_t i = 0; i < kBlock; ++i) {
        if (mine[i] == static_cast<unsigned char>((i % 97) + 1)) ++intact;
    }
    st->intactBytes.store(intact);
    st->doneCount.fetch_add(1);
}

} // namespace

TEST(FiberArenaIsolation, parkedFiberArenaBlockSurvivesCoHostedReset) {
#if defined(_WIN32)
    _putenv_s("CAJETA_CARRIERS", "1");
#else
    setenv("CAJETA_CARRIERS", "1", 1);               // before the pool starts
#endif
    Stage st;
    st.lock = __cajeta_lock_new();
    st.cv = __cajeta_condvar_new();
    ASSERT_NE(st.lock, nullptr);
    ASSERT_NE(st.cv, nullptr);

    void* slotA = nullptr;
    void* slotB = nullptr;
    __cajeta_task_run(&st, fiberB, &slotB);
    __cajeta_task_run(&st, fiberA, &slotA);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (st.doneCount.load() < 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(st.doneCount.load(), 2) << "fibers deadlocked / never finished";
    EXPECT_EQ(st.intactBytes.load(), (int) kBlock)
        << "a co-hosted fiber's arena reset reclaimed a parked fiber's "
           "live allocation (" << kBlock - st.intactBytes.load()
        << " of " << kBlock << " bytes corrupted)";
}
