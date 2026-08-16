//
// Sync-lowering MVP for the structured-concurrency keywords (docs/specification/concurrent/Concurrency.md):
// async / await / spawn / scope / detach. With no real scheduler yet, every
// spawn runs inline and finishes before the surrounding code continues —
// await/spawn collapse to direct calls, scope is just `{ ... }`, detach
// evaluates-and-discards. These tests prove the grammar and AST plumbing are
// in place end-to-end; the scheduler / Task<T> wrapping / state-machine
// lowering land in future phases without changing the surface syntax.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// `async` modifier parses on a method; the body codegens as a regular
// function. Calling it directly returns the inner value.

// `await` parses and passes the inner value through unchanged.

// `spawn` parses, runs the call inline (sync lowering), and materializes a
// Task<int32> wrapper that `await` unwraps. Bare `spawn` returns a
// Task<T>* now — bare integer destinations would be a type error, so the
// canonical form goes through `await`.

// `scope { ... }` is a statement that owns its contents. In the sync MVP
// it's just a block — locals declared inside drop at the closing `}`,
// same as any block. Verifies the parser routes SCOPE through to the
// block grammar correctly.

// R3-A: spawn of an async fn that takes one argument. Arg is evaluated
// at the spawn site (main thread), captured into the context struct,
// and read by the trampoline on the worker. Proves the context-capture
// pipeline carries primitive values correctly across the thread boundary.
TEST(AsyncSyntaxTests, spawnPassesOneArgument) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 doubled(int32 x) { return x + x; }\n"
        "    public static int32 run() { return await spawn doubled(21); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R4: smoke-test the fiber-aware lock_acquire path. A fiber acquires a
// freshly-allocated (uncontended) lock and releases it; the lock then
// gets reused by a second fiber. Under R3-B's plain pthread_mutex_lock
// this works trivially; under R4 the fiber goes through the new
// fiber-branch in __cajeta_lock_acquire (check `__cajeta_current_fiber`,
// take the lock's own mutex, check `held`, mark held=1). If the struct
// layout or check is broken, this test catches it.
//
// Deterministic fiber-on-fiber contention would require storing Task<T>
// in a user variable so two tasks can be spawned before either is awaited
// — and Task<T> isn't yet a user-resolvable type (it's only synthesized
// by the compiler at spawn sites). A more probative contention test
// lands once `Task<T>` is exposed as a known template.

// R4: a fiber holds a lock while it yields (via an inner await). The
// main thread then tries to acquire the same lock — it's NOT a fiber,
// so it uses the cond_wait path of __cajeta_lock_acquire. Either it
// blocks until the worker fiber resumes and releases, or it gets the
// uncontended fast path if the worker already finished. Both flows
// have to work for the test to pass.

// R5-A': implicit function-body scope. Even without an explicit
// `scope { ... }`, the function body itself is a scope — every unawaited
// spawn at the top level gets registered and joined at the function's
// closing brace.
//
// That join point is the LAST thing run() does, which is the crucial
// difference from scopeWaitsForUnawaitedSpawns: there, the explicit
// `scope }` join lands mid-function, so main can safely lockDestroy(h)
// AFTER it. Here there is no in-function statement past the implicit
// join, so main has nowhere safe to destroy a lock the worker still
// uses. So the worker owns the lock's entire lifetime — it acquires,
// releases, and destroys h as its final act — and the implicit
// function-body join makes run() wait for all of that before returning.
// If the implicit scope did NOT join, the worker would be orphaned and
// torn down mid-flight at JIT teardown (crash/hang), so a clean return
// of 42 proves the join fired.
//
// (Earlier this destroyed h in main BEFORE the implicit join — a
// use-after-free, since the cooperatively-scheduled worker only runs at
// the join and then dereferenced the freed lock. Benign on glibc but a
// hard segfault on mingw winpthreads; valgrind flagged the freed-lock
// pthread_mutex_lock read.)
TEST(AsyncSyntaxTests, implicitFunctionBodyScopeJoinsSpawn) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 yielder() { return 0; }\n"
        "    public static async int32 worker(pointer h) {\n"
        "        Cajeta.lockAcquire(h);\n"
        "        int32 dummy = await spawn yielder();\n"
        "        Cajeta.lockRelease(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        Cajeta.lockAcquire(h);\n"
        "        spawn worker(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R5-A: scope waits for unawaited spawns before letting control past `}`.
// The fiber acquires a lock, does some yielding work, then releases.
// Main holds the lock initially; main releases it inside the scope so
// the fiber can proceed; the scope's closing brace MUST wait until the
// fiber has run lockRelease(h) before main runs lockDestroy(h). If the
// scope didn't wait, lockDestroy would race with the fiber's lock ops
// and likely crash. A passing test = scope_exit successfully joined.
TEST(AsyncSyntaxTests, scopeWaitsForUnawaitedSpawns) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 yielder() { return 0; }\n"
        "    public static async int32 worker(pointer h) {\n"
        "        Cajeta.lockAcquire(h);\n"
        "        int32 dummy = await spawn yielder();\n"
        "        Cajeta.lockRelease(h);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        Cajeta.lockAcquire(h);\n"
        "        scope {\n"
        "            spawn worker(h);\n"
        "            Cajeta.lockRelease(h);\n"
        "        }\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R3-B: nested await — an async fn awaits another async fn. Under R2's
// single-worker model this would deadlock: the carrier blocks on cond_wait
// for the inner task's done flag, but the inner task is sitting on the
// queue with no worker free to run it. With stackful fibers + cooperative
// yield, the outer fiber parks itself when it awaits, the carrier picks
// up the inner fiber, that completes, parked fibers wake and the outer
// resumes. The test passing proves the fiber yield + wake-on-complete
// path actually moves through.
TEST(AsyncSyntaxTests, nestedAwaitDoesNotDeadlock) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 inner() { return 7; }\n"
        "    public static async int32 outer() {\n"
        "        int32 v = await spawn inner();\n"
        "        return v * 6;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return await spawn outer();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R3-A: spawn passing multiple arguments. Verifies the per-field ctx
// struct stores + loads work in arg order — a swap of two slots would
// produce the wrong subtraction result.
TEST(AsyncSyntaxTests, spawnPassesMultipleArguments) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 sub(int32 a, int32 b) { return a - b; }\n"
        "    public static int32 run() { return await spawn sub(100, 17); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 83);
}

// R3-A: arg values evaluated at spawn site come from outer locals, not
// from constants. Confirms the ctx capture path reads each arg through
// its alloca → r-value coercion the same way regular method-call sites
// would. Without that load, the worker would see the slot ADDRESS in
// its arg slot and the cast/add would either error or produce garbage.
TEST(AsyncSyntaxTests, spawnArgsFromLocals) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 mul(int32 a, int32 b) { return a * b; }\n"
        "    public static int32 run() {\n"
        "        int32 x = 6;\n"
        "        int32 y = 7;\n"
        "        return await spawn mul(x, y);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R2: spawning multiple tasks back-to-back exercises the queue depth and
// proves the await/condvar wait correctly pairs with each task's done
// flag (not a single global "any task done" signal). If the wait predicate
// were shared across tasks, the first await would return as soon as any
// later task completed — likely the wrong value.
TEST(AsyncSyntaxTests, multipleSpawnsRunIndependently) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 one() { return 1; }\n"
        "    public static async int32 ten() { return 10; }\n"
        "    public static async int32 hundred() { return 100; }\n"
        "    public static int32 run() {\n"
        "        int32 a = await spawn one();\n"
        "        int32 b = await spawn ten();\n"
        "        int32 c = await spawn hundred();\n"
        "        return a + b + c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 111);
}

// R1: `spawn` materializes a heap-allocated Task<T> wrapper whose value
// field carries the result and done flag is set true. The await unwraps
// the value through a struct-GEP — proving the wrapper actually exists
// (not pass-through) by exercising a chained spawn-then-await across a
// local binding. If R1's Task<T> codegen were missing, the local would
// hold an i32 (not a Task<int32>*) and the second await would type-error.
TEST(AsyncSyntaxTests, taskWrapperIsHeapAllocated) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 21; }\n"
        "    public static int32 run() {\n"
        "        int32 v = await spawn compute();\n"
        "        return v + v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `detach expr` parses and evaluates the inner expression for its side
// effects, returning no value to the surrounding context. The MVP runs
// it inline. Verifies the detach grammar + dispatch + codegen path.

// Probe: spawn with a class-instance arg that came from a class-typed
// ARRAY read (the case (#1) in the parallel-driver fork attempt). If
// the array-element read returns the slot pointer rather than the
// stored class instance, the spawn worker sees a corrupted share and
// mutations appear lost.
TEST(AsyncSyntaxTests, spawnClassArrayElementArgPropagates) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public void inc() { this.n = this.n + 1; }\n"
        "    public int32 get() { return this.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 bump(Counter c) {\n"
        "        c.inc();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter[] arr = heap Counter[2];\n"
        "        arr[0] = heap Counter();\n"
        "        int32 dummy = await spawn bump(arr[0]);\n"
        "        return arr[0].get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Probe: same shape, but with a named-local intermediate. This is the
// workaround pattern the parallel driver fell back to. If this passes
// while spawnClassArrayElementArgPropagates fails, it confirms the bug
// is at the array-element-read site, not at the spawn arg-capture site.
TEST(AsyncSyntaxTests, spawnClassArrayElementWithNamedLocalArg) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public void inc() { this.n = this.n + 1; }\n"
        "    public int32 get() { return this.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 bump(Counter c) {\n"
        "        c.inc();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter[] arr = heap Counter[2];\n"
        "        arr[0] = heap Counter();\n"
        "        Counter c = arr[0];\n"
        "        int32 dummy = await spawn bump(c);\n"
        "        return arr[0].get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Probe: spawn with a stdlib Stream<T> arg, where the worker calls
// .next() in a loop. This is the SHAPE that hung in the parallel-driver
// fork attempt. If next() pulls correctly inside the worker AND the
// orchestrator sees the share exhausted afterwards, mutations are
// propagating.
TEST(AsyncSyntaxTests, spawnStreamArgWorkerAdvancesShared) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class D {\n"
        "    public static async int32 drain(Stream<int32> s) {\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> o = s.next();\n"
        "        while (o.isPresent()) {\n"
        "            sum = sum + o.get();\n"
        "            o = s.next();\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(1);\n"
        "        list.add(2);\n"
        "        list.add(3);\n"
        "        Stream<int32> s #= list.stream();\n"
        "        return await spawn drain(s);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// Probe: full parallel-reduce shape — spawn workers each draining a
// stream share into a partial slot, with a fn lambda for the
// accumulator. This is the parallel-driver fork/join pattern.
TEST(AsyncSyntaxTests, spawnForkJoinStreamReduceShape) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.lang.stream.Stream;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.stream.Splittable;\n"
        "public final class D {\n"
        "    public static async int32 work(Stream<int32> share, int32 slot, int32[] partials, (int32, int32) -> int32 fn) {\n"
        "        int32 acc = 0;\n"
        "        Optional<int32> o = share.next();\n"
        "        while (o.isPresent()) {\n"
        "            acc = fn(acc, o.get());\n"
        "            o = share.next();\n"
        "        }\n"
        "        partials[slot] = acc;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> list = heap ArrayList<int32>();\n"
        "        list.add(1);\n"
        "        list.add(2);\n"
        "        list.add(3);\n"
        "        list.add(4);\n"
        "        list.add(5);\n"
        "        list.add(6);\n"
        "        list.add(7);\n"
        "        list.add(8);\n"
        "        Splittable<int32> source #= list.stream();\n"
        "        Stream<int32> share = (Stream<int32>) source.trySplit();\n"
        "        int32[] partials = heap int32[2];\n"
        "        (int32, int32) -> int32 fn = (a, b) -> a + b;\n"
        "        scope {\n"
        "            spawn work(share, 0, partials, fn);\n"
        "        }\n"
        "        int32 tail = 0;\n"
        "        Optional<int32> o = source.next();\n"
        "        while (o.isPresent()) {\n"
        "            tail = fn(tail, o.get());\n"
        "            o = source.next();\n"
        "        }\n"
        "        partials[1] = tail;\n"
        "        return partials[0] + partials[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 36);
}

// Probe: real fork/join shape — spawn N workers, each writing to a
// separate slot of a shared int32 array, then the orchestrator
// reduces partials. This is the structural inverse of the
// parallel-reduce driver pattern.
TEST(AsyncSyntaxTests, spawnForkJoinPartialsAggregation) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 work(int32[] partials, int32 slot, int32 v) {\n"
        "        partials[slot] = v * v;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] partials = heap int32[4];\n"
        "        scope {\n"
        "            spawn work(partials, 0, 1);\n"
        "            spawn work(partials, 1, 2);\n"
        "            spawn work(partials, 2, 3);\n"
        "            spawn work(partials, 3, 4);\n"
        "        }\n"
        "        int32 sum = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            sum = sum + partials[i];\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Probe: spawn with class-instance arg PLUS additional args (primitive,
// array, lambda). Reproduces the parallel-driver spawn-worker call shape
// where the worker took (share, slot, partials, seed, fn).
TEST(AsyncSyntaxTests, spawnClassInstanceArgPlusOtherArgs) {
    auto src =
        "package test;\n"
        "public class Cursor {\n"
        "    public int32 idx;\n"
        "    public Cursor() { this.idx = 0; }\n"
        "    public void step() { this.idx = this.idx + 1; }\n"
        "    public int32 read() { return this.idx; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 work(Cursor c, int32 times, int32[] sink, int32 slot) {\n"
        "        int32 i = 0;\n"
        "        while (i < times) {\n"
        "            c.step();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        sink[slot] = c.read();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cursor c = heap Cursor();\n"
        "        int32[] sink = heap int32[2];\n"
        "        int32 dummy = await spawn work(c, 5, sink, 1);\n"
        "        return c.read() * 10 + sink[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 55);
}

// Probe: templated class-instance arg to spawn. Same shape as
// spawnClassInstanceArgMutationVisible, but the captured class is
// generic — the case that caused stream-parallel fork to hang.
TEST(AsyncSyntaxTests, spawnTemplatedClassInstanceArgMutationVisible) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public int32 hits;\n"
        "    public Box() { this.hits = 0; }\n"
        "    public void bump() { this.hits = this.hits + 1; }\n"
        "    public int32 get() { return this.hits; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 work(Box<int32> b) {\n"
        "        b.bump();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>();\n"
        "        int32 dummy = await spawn work(b);\n"
        "        return b.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Probe: spawn with a class-instance arg. The worker mutates a field of
// the heap-allocated class instance; the orchestrator reads the field
// back after the spawn joins. Both should see the same heap body, so
// the read must observe the worker's mutation.
TEST(AsyncSyntaxTests, spawnClassInstanceArgMutationVisible) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public void inc() { this.n = this.n + 1; }\n"
        "    public int32 get() { return this.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 bump(Counter c) {\n"
        "        c.inc();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        int32 dummy = await spawn bump(c);\n"
        "        return c.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
