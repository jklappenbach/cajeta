# Cajeta concurrency / release state — handoff

_Last updated: 2026-06-12, after v0.7.0-rc3. Branch: `feature/cvm`._

## TL;DR

Three independent problems, NOT one. They have been getting conflated under
"the tests crash." Keep them separate:

| Leg | rc3 result | Root cause | Status |
|-----|-----------|-----------|--------|
| **x86_64-linux** (hard gate) | **474/474 PASS, 0 crash** | — | ✅ GREEN |
| **aarch64-linux** | ~~58 crashes~~ → **0** | sret call-site ABI bug (x8 vs x0); see [[sret-callsite-abi-arm]] | ✅ FIXED (run 27457698391: 0 crashes, carrier probe 10/10 exit 0) |
| **aarch64-apple-darwin** | 424 fail, 0 crash, ~14s each | JIT can't resolve `__trunctfdf2`, `__fixtfdi` | ❌ open |
| **x86_64-w64-mingw32** | (rc2: 425 fail) | almost certainly same JIT-symbol class as macOS | ❌ open (rc3 pending) |

`continue-on-error` masks every leg except x86_64-linux, so the run shows
mostly green even though 3/4 legs fail. See [[release-ci-fake-green-masking]].

## What is already FIXED and committed (feature/cvm, since rc2)

- `c3628d0` **Task-completion UAF** — `__cajeta_task_complete` published
  `*done_addr = 1` before the carrier's `*f->slot_ptr = NULL` write into Task
  memory; under preemption that write hit freed/recycled heap. Now nulled
  under the mutex BEFORE done. This was the SIGABRT/134 "corrupted size vs.
  prev_size" class. **Confirmed gone**: rc3 has zero 134s on any leg; x86 went
  from 3 crashes → 0; ARM's await/task/spawn-drop crashes (TaskTyping,
  SpawnDrop.spawnInsideInnerScope, AsyncSyntax.mainThreadWaitsOnFiberHolder,
  etc.) are all gone. See [[task-completion-uaf-race]].
- `e5b5f1d` **Guard-paged fiber stacks** — POSIX mmap + PROT_NONE low page so
  overflow faults cleanly. (Did not change the crash count — the remaining ARM
  crashes are not stack overflows — but it makes the next one diagnosable.)
- `c366e5b` migrate off deprecated `llvm::BranchInst`.
- `7d02e6a` **KEEP_LOGS + per-leg `test-logs-<target>` artifacts** — this is how
  the macOS root cause below was found without a Mac. `cajeta_tests.sh` brief
  mode means PASS banners have no test name, but FAILURE detail (gtest messages,
  thrown-exception text) IS captured.

## Problem 1 — aarch64-linux: 58 SIGSEGV — NOT concurrency, it's ARM JIT codegen (OPEN)

**MAJOR CORRECTION (2026-06-13).** The "weak-memory data race in parallel-stream
dispatch" diagnosis below is **WRONG**. Disproved two ways:

1. **ASan on x86 is clean on EVERY parallel-stream test**, including the
   heap-owning collect-into-ArrayList ones (6/6 PASS, 0 errors). ASan's
   allocator catches double-free/UAF regardless of architecture, so there is
   no heap-lifetime bug. (The makecontext/swapcontext ASan caveat only affects
   *stack* tracking, not heap free/UAF.)
2. **Tests that never spawn a fiber still crash on ARM**: `parallelReduceOn
   TinySourceStillCorrect` (`{10,20,30}` → pickSplitCount=1 → sequential walk,
   no spawn), `sequentialBeforeTakeClearsFlag` / `sequentialFlipBackPreserves
   Count` (`.sequential()` clears the flag → sequential), `parallelFindFirst
   RejectsStatefulInChain` (`.take().parallel()` → throws before forking). No
   fiber, no carrier pool, single-threaded — a data race is impossible.

**Corrected diagnosis: an ARM-specific JIT codegen/lowering bug in the
`ParallelDriver` path.** `.parallel()` dispatches through ParallelDriver even
when it falls back to a sequential walk, so the discriminator is "went through
ParallelDriver", not "ran in parallel". The fault is in `free` (libc aarch64
+0x41cc0) ~4 frames deep in JIT'd code on the MAIN thread, freeing a garbage
pointer (recurring `0x2bf5282400a` ≈ a truncated/mis-typed 42-bit pointer; also
nil) during stream/template-object teardown. Heavy template use
(`reduceParallel<T>`, `Optional<T>`, `Stream<T>[]`) + drop — possible link to
[[redblack-object-autoextend-segv]] (Object auto-extend on template
instantiations). The UAF fix (c3628d0) was real and removed the SIGABRT/134
class, but is unrelated to this remaining SIGSEGV class.

**Next step in flight:** ARM dry-run run 27457001339 carries a TEMP debug step
(`TEMP DEBUG ARM crash backtrace`, aarch64 only) that runs `parallelReduceSums
Correctly` under gdb with `CAJETA_JIT_GDB=1` (LLVM ORC GDB-JIT registration) to
SYMBOLIZE the JIT frames + a CAJETA_CARRIERS=1-vs-4 determinism probe. The
symbolized frame names the exact drop/free function to inspect. Remove the
debug step (release.yml) before GA.

### (superseded) original concurrency hypothesis — kept for history

- All exit 139, concentrated in `ParallelStreamP1Tests`,
  `HashMapStreamParallelTests`, `ParallelDispatchCorrelationTests`, plus 2
  `AsyncSyntaxTests` spawn-**stream** cases. The pure await/spawn/task-drop
  crashes are GONE (UAF fix). So this is specifically the **parallel stream
  partition/dispatch/collect** path, not the Task lifecycle.
- x86_64-linux runs the identical code and JIT and passes 474/474. Same OS,
  same build. The discriminator is the **CPU architecture**.
- **Leading hypothesis: a data race in the parallel-stream dispatch that x86's
  TSO memory model hides and aarch64's weak ordering exposes.** Candidates:
  missing acquire/release on the shared result accumulator or partition/`done`
  state when carriers combine partials; or an under-fenced handoff in
  `__cajeta_publish_ready` / `__cajeta_steal_one` / the `home_carrier` pinning.
  The Chase-Lev deque itself (cajeta_runtime.c ~1044-1135) audits clean
  (correct SeqCst fences); look ABOVE it at the stream driver and the
  cross-carrier combine.
- The parallel-stream **driver** is mostly in cajeta stdlib + codegen, not just
  the C runtime — find it via `ParallelStreamP1Tests.cpp` (test/parser/) →
  what intrinsic/dispatch it exercises → `runtime/src/cajeta/lang/stream/` and
  the `__cajeta_*` parallel-dispatch natives.
- **ARM backtrace data (from the SIGSEGV handler, dry-run run 27454371810):**
  - **ALL 58 crashes are on the main/orchestrator thread** (`carrier=-1 fiber=0`),
    never a worker carrier. The orchestrator reads worker-produced state and
    faults.
  - Fault addresses: 11× null, 5× a repeated garbage pointer `0x2bf5282400a`,
    rest assorted stack-like highs. **30 of 58 pass through one libc memory
    function** (offset +0x41cc0, likely free/memcpy) — the shape of a
    double-free / free-of-corrupted-pointer during orchestrator teardown.
  - Faulting frames are JIT'd (unsymbolized bare addresses) below the native
    JIT-invoke trampoline + the gtest TestBody.
  - Stream split (ArrayStream.trySplit) is clean (disjoint idx/limit, shared
    read-only buffer); scope_exit / task_wait / task_complete establish correct
    mutex happens-before. `parallelReduceSumsCorrectly` (int elements, no heap
    ownership) crashes too → NOT element-ownership; suspect is the `shares`/
    `partials` arrays or the **Task objects** dropped at orchestrator scope
    cleanup (double-drop across the spawn boundary).
  - **Working hypothesis: double-drop** — an object dropped once by the worker
    fiber's unwind and again by the orchestrator's scope cleanup; sequential
    (join-ordered) double-free that x86 glibc tolerates/ SIGABRTs but ARM turns
    into a SIGSEGV in free. Pending the drop-chain dump (added to the SIGSEGV
    handler, run 27455672393) to name the exact object + cajeta source tag.
- **Cannot reproduce on this x86 box** (even CAJETA_CARRIERS=16). Options:
  (a) run under `qemu-aarch64` or an ARM box; (b) audit every shared write in
  the parallel-stream combine path for missing `__atomic_*` acquire/release and
  add a ThreadSanitizer build (TSan finds these on x86 even when TSO hides the
  crash — **strongly recommended first move**); (c) pull the ARM run-logs
  artifact for any SIGSEGV detail (note: 139 leaves no gtest backtrace; the
  runtime has a SIGABRT handler but not a SIGSEGV one — consider adding a
  SIGSEGV backtrace handler as a diagnostic commit).

## Problem 2 — macOS (and likely Windows): missing JIT builtins (OPEN, tractable)

- **Exact cause (from rc3 artifact `test-logs-aarch64-apple-darwin`):** all 424
  failures are `JIT session error: Symbols not found: [ __trunctfdf2,
  __fixtfdi ]` (233 + 191 = 424, exact match). These are compiler-rt 128-bit
  float helpers: `__trunctfdf2` = `long double`/`fp128` → `double`; `__fixtfdi`
  = `fp128` → i64. The stdlib pulls in fp128/`long double` conversions, so EVERY
  JIT'd program fails to materialize → every test fails (even `intAdd`). The
  ~14s per test is just the JIT compile before the link fails.
- This is NOT the concurrency bug and NOT `__emutls_get_address` (the
  release.yml comment guessed wrong). It is the same CLASS as the prior
  "TLS/OpenSSL link gap" — symbols the JIT's resolver doesn't expose. See
  [[emit-exe-linux-bugs]].
- **Fix direction:** register these compiler-rt builtins in the LLJIT symbol
  resolver (define/forward `__trunctfdf2`, `__fixtfdi`, and probably the rest of
  the `__*tf*` family for completeness), or link the JIT process against
  compiler-rt builtins so dlsym resolves them. Look at where the JIT defines its
  symbol generator / `DefinitionGenerator` (search src/ for LLJIT setup,
  `DynamicLibrarySearchGenerator`, `absoluteSymbols`). On x86-linux these
  resolve from the host process's libgcc, which is why x86 is green.
- Windows rc3 leg still running; rc2 had 425 failures — expect a sibling
  symbol-resolution gap (mingw libgcc names). Pull `test-logs-x86_64-w64-mingw32`
  when rc3 finishes to confirm.

## Repro techniques that work (on this x86 box)

- **Starvation repro** (`/tmp/crashrepro2/run.sh`, recreate if gone): pin to 4
  CPUs with `taskset -c 0-3`, run the crashing tests as ~8 concurrent
  one-test processes. This is how the UAF was caught on x86.
- **Window-stretch + detector**: temporarily `usleep` in the suspect window +
  a slot/state-integrity check + `MALLOC_PERTURB_=170` makes a lifecycle race
  fire even solo. Strip the diagnostics before committing.
- **Filters run SERIAL in ONE process** in cajeta_tests.sh; use `PARALLEL=1` or
  per-test processes to repro parallel bugs.
- **TSan build** is the recommended next tool for Problem 1 — it surfaces the
  weak-memory race on x86 without needing ARM hardware.

## Release state

- GA v0.7.0 is HELD. rc3 = `5d7d038` on feature/cvm (tag `v0.7.0-rc3`),
  commits since rc2: c366e5b, c3628d0, e5b5f1d, 7d02e6a, 5d7d038.
- feature/cvm is 27 ahead / 0 behind origin/cajeta-xpu → clean FF when ready.
- cvm install validated end-to-end against rc2 published assets (redirect fix
  works). See [[cvm-state]].
- Do NOT cut GA until Problems 1+2 are fixed or the masking is an explicit
  eyes-open decision. x86_64-linux is the only honestly-gated leg today.
