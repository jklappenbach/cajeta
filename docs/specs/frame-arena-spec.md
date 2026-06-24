# Frame Bump Arena for Non-Escaping Owned Locals — Spec

## 1. Definition

### 1.1 Purpose
Eliminate the per-object `malloc` / `free` and live-set table traffic for heap
allocations whose lifetime is provably bounded by the current function frame.
Such objects are allocated from a per-frame **bump arena** (pointer increment, no
live-set registration) and reclaimed by an **O(1) arena reset** at frame exit
instead of an individual `free` per object.

### 1.2 Problem
`hashmap-string` profiling (2026-06-23) shows cajeta at 5.03 ms median vs Java
2.80 ms and C++ (ankerl) 1.74 ms. The gap is entirely allocation cost. Per
transient String, cajeta pays: `malloc` + `free` (~15–30 ns each) **plus** a
live-set hash insert on alloc and a hash claim on free — overhead neither
competitor pays (C++ uses SSO + RAII, Java uses TLAB bump + bulk GC). The
benchmark's lookup loop builds 30 000 throwaway `q = "key"+j` Strings that are
borrowed into `get()` and dropped at loop scope; each currently costs two
allocations, two frees, and four live-set table ops.

### 1.3 Approach
A thread-local bump arena managed as a **stack of marks aligned with lexical
scopes** (the same boundaries where the drop chain fires today). On entering a
scope that contains arena-eligible allocations, codegen takes a **mark**;
eligible allocations bump the arena pointer and are **not** live-set tracked and
get **no individual drop entry**; on leaving that scope the arena is **reset to
the mark**, reclaiming every object allocated in the scope at once. This is the
no-GC analog of Java's bulk reclaim.

**Reset is scope-aligned, NOT function-aligned.** A loop-body local is reclaimed
at loop-body scope exit — i.e. once per iteration — exactly as its drop entry
fires today. This bounds arena residency to the *deepest live scope's* legitimate
working set regardless of loop length, so an arbitrarily long loop never
accumulates (see § 6).

### 1.4 Constraints
- **Soundness is paramount.** An object placed in the arena that outlives the
  frame is a use-after-free on reset. Eligibility must be a *conservative*
  compile-time guarantee — when in doubt, fall back to the existing heap+live-set
  path (correct, just slower).
- **No ABI/type change.** Arena objects are byte-identical to heap objects
  (same String/array layout); only their *provenance* and *reclamation* differ.
- **Reuse existing signals.** The drop chain already marks owned locals and
  deactivates entries on `return` / `#`-transfer; escape analysis builds on that
  rather than inventing a parallel ownership model.
- **Single-threaded fast path preserved.** The arena is thread-local; spawning a
  worker thread must not corrupt another thread's arena.

### 1.5 Non-goals
- Not a general garbage collector or region-inference system.
- Not changing escape *semantics* of the language (no new syntax).
- Not arena-allocating objects that escape (returned, `#`-transferred, stored in
  a longer-lived field/array/container) — those keep the heap+live-set path.
- Not eliminating allocation for *stored* map keys (they escape) — that is the
  separate "inline keys in the map" lever, out of scope here.
- Not the int→string-into-buffer optimization (separate, complementary lever).

## 2. Arena runtime

### 2.1 Requirements
A thread-local growable bump allocator with mark/alloc/reset, holding objects
byte-compatible with the heap allocators (`__cajeta_alloc` /
`__cajeta_alloc_uninit` / array headers). Objects are never individually freed and
never entered into the live-set. The arena grows by chunking when a bump would
overflow the current slab; reset returns the bump pointer to a saved mark
(freeing whole over-mark chunks back to a free-list or the OS, retaining the
first slab for reuse).

### 2.2 Use cases
- 2.2.1 As the codegen, when I emit an arena-eligible allocation of N bytes, I
  call `__cajeta_arena_alloc(N)` and get a zeroed (or uninit variant) block that
  behaves exactly like a heap block for all readers.
- 2.2.2 As the entry of a scope that may arena-allocate, I call
  `__cajeta_arena_mark()` and stash the returned mark in a local.
- 2.2.3 As the exit of that scope (every edge that leaves it — normal fallthrough,
  `break`/`continue`, `return`), I call `__cajeta_arena_reset(mark)` so all objects
  allocated in the scope are reclaimed at once.
- 2.2.4 As a deeply recursive or large-loop function, when many objects are
  arena-allocated, the arena grows by chunks and never blows a fixed cap; reset
  reclaims them in O(1) per chunk.
- 2.2.5 As the runtime, when a second thread is spawned, each thread has its own
  arena so concurrent arena use cannot corrupt another frame's objects.

## 3. Escape analysis (eligibility)

### 3.1 Requirements
A local binding is **arena-eligible** iff the compiler can prove its value does
not escape the current frame. Conservative rule — eligible only when ALL hold:
- the initializer is a fresh owned allocation the local owns (gets an owned drop
  entry today: String concat result, allocating String method, `heap T(...)`,
  `heap T[]`), AND
- the local is **never** the operand of a move (`#local`), AND
- the local is **never** returned (`return local`, or returned inside a larger
  expression), AND
- the local is **never** stored into a field, array element, or other
  heap-reachable location, AND
- the local is **never** passed to a `#`-parameter (callee takes ownership), AND
- the local is **never** aliased to another binding that itself escapes
  (transitive; conservatively, any alias to a class/array ref makes it ineligible
  unless the alias is also proven frame-local).
Anything not provably eligible falls back to heap+live-set (status quo).

### 3.2 Use cases
- 3.2.1 As a developer writing `String q = "key"+j; int v = m.get(q);` in a loop,
  when `q` is only borrowed into `get` and dropped at scope, the compiler marks
  `q`'s allocation arena-eligible — no malloc, no live-set, no per-object free.
- 3.2.2 As a developer writing `String k = "key"+i; m.put(#k, i);`, when `k` is
  `#`-transferred into the map, the compiler marks `k` **ineligible** (escapes);
  it stays heap+live-set so the map can own and later free it.
- 3.2.3 As a developer writing `String s = build(); return s;`, when `s` is
  returned, the compiler marks `s` ineligible (escapes to caller).
- 3.2.4 As a developer writing `this.name = "x"+y;` (store into a field), the
  stored allocation is ineligible.
- 3.2.5 As a developer writing code the analysis cannot prove safe, the compiler
  conservatively keeps the heap path — correct, never a UAF, just no speedup.

## 4. Codegen integration

### 4.1 Requirements
- 4.1.1 Per **scope**, emit the mark at scope entry only if the scope (transitively)
  contains ≥1 arena-eligible allocation; emit the matching reset on every edge that
  leaves the scope (fallthrough, `break`, `continue`, `return`, throw), exactly once
  per edge. Marks/resets nest LIFO with the scope structure and the drop chain. For
  a loop body this places the reset at the iteration boundary (§ 6.1.1).
- 4.1.2 An arena-eligible allocation site routes to `__cajeta_arena_alloc`
  (zeroed) or an uninit variant, skips `__cajeta_live_set_add`, and registers **no
  drop entry** (the arena reset is its reclamation).
- 4.1.3 String concat and allocating String methods honor eligibility: when their
  result binds to an eligible local, the wrapper (and any heap byte buffer for
  non-SSO results) come from the arena.
- 4.1.4 Exception/throw unwinding must also reset the arena (or the arena reset
  must be unwind-safe) so a thrown frame does not leak its arena chunks
  permanently. (If full unwind integration is out of MVP scope, document the
  leak-on-throw as a known limitation and ensure it cannot cause UAF.)

### 4.2 Use cases
- 4.2.1 As codegen, when a function has eligible allocations, I bracket the body
  with mark/reset so every exit path reclaims the frame's arena objects.
- 4.2.2 As codegen, when an eligible String concat is the loop body's only
  allocation, the loop runs with zero malloc/free and zero live-set ops.
- 4.2.3 As codegen for a function with no eligible allocations, I emit no
  mark/reset and behavior is byte-identical to today.

## 5. Correctness & verification

### 5.1 Requirements
- 5.1.1 No use-after-free: an arena object must never be read after the frame that
  allocated it returns. Guaranteed by eligibility (3.1) + reset on every exit.
- 5.1.2 No double-free: arena objects are not in the live-set, so a stray
  `dropValue` / `free_array` / `free` on one is a safe no-op (live-set claim
  fails) — verify this holds for the array and class drop paths.
- 5.1.3 Parity: existing String/collection/codec/parser tests pass unchanged
  (the arena is an allocation-strategy optimization, not a semantic change).
- 5.1.4 Measurable win: hashmap-string median improves materially and the bench
  result remains correct (sum cross-check intact, live-set bounded).

### 5.2 Use cases
- 5.2.1 As the test suite, when I run the ownership leak probes, arena-eligible
  transient strings show zero net live-set growth (they never enter it) and no
  leak (reset reclaims them).
- 5.2.2 As the test suite, when I run a probe that `#`-transfers a key into a map
  and drops the map, the key is still freed exactly once (it took the heap path).
- 5.2.3 As the benchmark, when I run hashmap-string, median moves toward Java/C++
  and `checkResult` still passes.

## 6. Bounded memory (no unbounded growth)

### 6.1 Requirements
- 6.1.1 **Scope-aligned reset.** The arena mark/reset bracket the *innermost scope*
  the eligible local belongs to, matching where its drop entry fires today. A
  local in a loop body is reclaimed every iteration; arena residency for that loop
  is bounded by one iteration's eligible allocations, independent of iteration
  count. There is therefore no accumulation across a long/infinite loop.
- 6.1.2 **Parity with heap liveness.** Because reset boundaries equal today's drop
  boundaries, the arena's memory high-water mark for any program equals the
  heap+drop-chain high-water mark for the same program — the optimization changes
  allocation *cost*, not object *liveness*.
- 6.1.3 **Reset is O(1).** `__cajeta_arena_reset(mark)` is a single bump-pointer
  restore — objects are abandoned in place and their bytes reused on the next
  bump. There is NO per-object free and NO per-object drop in a reset. A
  per-iteration reset therefore costs ~two pointer ops regardless of how many
  objects the iteration allocated; the per-object `malloc`/`free`/live-set cost is
  eliminated outright, not deferred.
- 6.1.4 **Reset at the object's true liveness scope.** Each eligible local resets
  at the innermost scope its liveness permits: a loop-body-local that does not
  cross iterations resets per iteration (memory bounded to one iteration); an
  object that legitimately lives across the loop but not beyond the method resets
  at method scope (that memory is genuinely required, not waste).
- 6.1.5 **Slab reuse with a retention cap.** The arena keeps freed slabs for reuse
  (so a hot loop does not re-`mmap` each iteration). Retained capacity is bounded
  by the peak scope working set. On reset, if retained-but-unused capacity exceeds
  a configurable threshold (default a few MB), the excess slabs are returned to the
  OS — a backstop "trim on reset" so a one-off deep scope does not pin a large slab
  for the process lifetime.
- 6.1.6 **No correctness dependence on the trim.** Trimming is a memory-pressure
  backstop only; correctness (no UAF, no OOM beyond the program's intrinsic
  working set) holds from scope-aligned reset alone.

## 7. Foreign-core reaper (deferred backstop)

### 7.1 Requirements
The inline reset (6.1.3) is O(1) and does the common case with zero hot-path cost,
so it needs no help for memory-only objects. Two reclamation tasks ARE genuinely
expensive and, when present, must be kept off the hot core:
- 7.1.1 **Slab `munmap`** under the retention-cap trim (6.1.5) — a syscall (~µs).
- 7.1.2 **Batched destructors** — once the arena holds objects with real `~T()`
  side effects (not strings), scope exit must run code per object; those are queued
  and run as a batch rather than per object on the hot path.
For both, a **reaper thread pinned to a foreign core** drains a hand-off queue
(full slabs to `munmap`; destructor batches to run) while the hot core keeps
bumping a fresh slab. The inline reset stays O(1); only the syscall/CPU-heavy work
is offloaded.

### 7.2 Status — DEFERRED
Not in the MVP. It is load-bearing only once (a) we arena objects that have
destructors (§3.3.3 keeps those ineligible for now) or (b) sustained memory
pressure makes inline trim syscalls visible on the hot path. Until then the inline
O(1) reset + occasional inline trim is sufficient. Specced here so the MVP's
arena/reset API is shaped to allow a later async drain without rework (e.g. reset
returns over-mark slabs to a thread-safe free-list that a reaper can later own).

### 7.3 Use cases
- 7.3.1 As a hot loop arena'ing string temporaries, when reset runs, no syscall and
  no destructor runs — pure O(1) pointer restore (reaper not involved).
- 7.3.2 As a future workload arena'ing destructor-bearing objects, when a scope
  exits, the destructor batch is enqueued to the reaper and the hot core continues;
  the reaper runs them on a foreign core.
- 7.3.3 As a memory-pressure trim, when reset would `munmap` excess slabs, the slabs
  are enqueued to the reaper instead of `munmap`-ing on the hot core.

### 6.2 Use cases
- 6.2.1 As an event loop `while (true) { String m = "tick "+n; log(m); }`, when
  each `m` is frame-local, the arena resets every iteration and steady-state arena
  memory is one message's worth — never grows with uptime.
- 6.2.2 As a function with one deep non-loop scope that legitimately builds a large
  amount of transient string data, when the scope exits the arena resets and (if
  over the retention cap) trims slabs back to the OS rather than pinning them.
- 6.2.3 As a hot short loop, when iterations reset to the same mark, the retained
  first slab is reused every iteration with no syscall — fast and bounded.
