# Cajeta on embedded — design + roadmap

This document scopes what it would take to make cajeta a serious option for embedded systems. It is **a roadmap, not a status report.** As of this writing, cajeta is comfortable on Linux-class single-board computers (Raspberry Pi, BeagleBone) — a 36 KB HelloWorld with full feature surface is competitive. It is **not** yet a fit for microcontroller-class targets (Cortex-M, RP2040, ESP32 bare-metal) where the runtime's pthread dependency, fixed-size live-set BSS, and constructor-init machinery all stand in the way. The plan below addresses those blockers in tiers so each phase lands independent value and the language never has a "embedded mode is broken" intermediate state.

The headline pitch: cajeta has memory safety without GC pauses, deterministic destruction via the drop chain, true monomorphization with no type erasure, structured concurrency that maps cleanly onto cooperative schedulers, and a syntax familiar to anyone with C / C++ / Java / TypeScript background. Those properties make a strong embedded language *in principle*. The work is making the runtime honor the constraints of each embedded tier.

> **Cross-references:** [`CompilerModes.md`](CompilerModes.md) for the existing mode presets, [`specification/lang/MemoryModel.md`](specification/lang/MemoryModel.md) for the drop-chain design, [`specification/lang/FieldOwnership.md`](specification/lang/FieldOwnership.md) for the live-set rationale, [`specification/concurrent/Concurrency.md`](specification/concurrent/Concurrency.md) for the concurrency primitives that need a threading abstraction layer.

---

## Table of contents

- [Target tiers](#target-tiers)
- [Why cajeta on embedded](#why-cajeta-on-embedded)
- [Current blockers](#current-blockers)
- [Roadmap](#roadmap)
  - [E1 — `--mode=embedded-linux`](#e1--mode-embedded-linux)
  - [E2 — Configurable live-set](#e2--configurable-live-set)
  - [E3 — Threading abstraction layer](#e3--threading-abstraction-layer)
  - [E4 — Allocator abstraction](#e4--allocator-abstraction)
  - [E5 — Explicit `__cajeta_init` (no constructor)](#e5--explicit-__cajeta_init-no-constructor)
  - [E6 — Stdlib feature gating](#e6--stdlib-feature-gating)
  - [E7 — Libc-free runtime](#e7--libc-free-runtime)
  - [E8 — Pluggable I/O](#e8--pluggable-io)
  - [E9 — Bare-metal target profiles](#e9--bare-metal-target-profiles)
  - [E10 — Heapless mode](#e10--heapless-mode)
- [Per-tier size targets](#per-tier-size-targets)
- [Comparisons](#comparisons)
- [Open questions](#open-questions)
- [Status](#status)

---

## Target tiers

Embedded is not one constraint envelope but four. The cajeta runtime needs different shapes at each.

| Tier | Class | Flash | RAM | OS | libc | Examples |
|---|---|---|---|---|---|---|
| **T1** | IoT Linux | ≥ 16 MB | ≥ 32 MB | yes (Linux) | yes (glibc/musl) | Raspberry Pi, BeagleBone, OpenWrt routers |
| **T2** | RTOS | 256 KB – 4 MB | 64 KB – 1 MB | yes (FreeRTOS / Zephyr / NuttX) | partial (newlib-nano) | ESP32, STM32H7, nRF53 |
| **T3** | Bare-metal MCU | 64 KB – 1 MB | 16 KB – 256 KB | no | partial / none | Cortex-M0/M3/M4, RP2040 |
| **T4** | Deeply embedded | < 64 KB | < 16 KB | no | none | Cortex-M0+, AVR, some RISC-V cores |

T1 works today. T2/T3 are the next-session targets. T4 is aspirational — cajeta's feature set (exceptions, drop chain, virtual dispatch) is fundamentally heavier than what fits in 64 KB; this tier may end up being "cajeta with a stripped subset" rather than full cajeta. We won't promise T4 until the path is concrete.

---

## Why cajeta on embedded

The standard embedded languages are C, C++, Rust (with `no_std`), and Zig. Each has trade-offs:

- **C** — small, predictable, ubiquitous. No memory safety; no type system to speak of; no ergonomic collections; manual everything.
- **C++** — full feature set; exceptions and RTTI are typically disabled (`-fno-exceptions -fno-rtti`); templates work but contribute to code bloat; standard library is partially usable.
- **Rust (`no_std`)** — memory-safe; tight binaries; community-curated embedded ecosystem. Borrow checker is great when it works and frustrating when it doesn't; macros and traits are powerful but increase compile times; learning curve is steep.
- **Zig** — small, comptime is excellent for embedded; manual error handling; relatively small ecosystem.

Cajeta sits in a different point of the design space:

- **Memory safety without GC** — drop chain is deterministic and bounded; no garbage collector to schedule.
- **Templates monomorphize** — no virtual function indirection unless the user opts in; class instances pass by pointer but vtable use is per-call-site.
- **Exceptions integrate with drops** — uncaught throws unwind the drop chain to the catching frame's watermark; recoverable vs. unrecoverable distinguish catchable errors from alarms.
- **Structured concurrency primitives** — `async fn`, `scope { ... }`, `spawn` map onto cooperative single-carrier schedulers naturally. An RTOS task is a cooperative carrier; cajeta fibers schedule onto it.
- **Familiar syntax** — anyone with Java / C# / TypeScript / C++ background can read cajeta in five minutes. No new mental model for ownership beyond `#`-transfer.
- **`@Native` FFI** — direct annotation-driven binding to C functions; no marshalling layer.

The pitch for embedded: write your firmware in a language that prevents use-after-free, double-free, and aliasing-mutation by construction; deterministic destruction; structured concurrency for RTOS task composition; full templating for type-safe register / peripheral abstractions; and a binary footprint competitive with C++ once the runtime is appropriately trimmed.

---

## Current blockers

Concrete things that prevent cajeta from running below tier T1 today:

1. **`pthread_mutex_t` everywhere in the runtime.** The live-set, the drop-chain count, the property map, the trace side-table, the fiber scheduler — all use pthread primitives. FreeRTOS and Zephyr don't have pthread; bare-metal has nothing.
2. **`__cajeta_live_set` is a 512 KB BSS block.** `CAJETA_LIVE_SET_CAPACITY = 1 << 16` slots × 8 bytes per pointer. Zero file impact, but 512 KB of RAM larger than the entire SRAM on most Cortex-M parts.
3. **Constructor-driven init.** `__cajeta_runtime_init` runs via `__attribute__((constructor))` on Linux's `.init_array` mechanism. Bare-metal toolchains may not honor this, depending on the startup file.
4. **Direct libc heap.** `__cajeta_alloc` calls `calloc` and `__cajeta_free_array` calls `free` (`runtime/native/cajeta_runtime.c`). Many MCU heaps are non-libc (FreeRTOS heap, TLSF, region allocators) or absent entirely.
5. **Output via libc `write` / `fprintf`.** All `print` / `println` routes through `__cajeta_log` → the static `__cajeta_emit`, which calls libc `write(fd, …)`; assorted runtime error paths additionally use `fprintf(stderr, …)` (e.g. the allocation-failure message). No POSIX I/O on bare-metal; the user wants UART, semihosting, RTT, ITM, or nothing. (E8 makes `__cajeta_emit` pluggable.)
6. **`backtrace(3)` for stack traces.** glibc-specific.
7. **`pthread_atfork` / signal handlers.** Used for the SIGABRT dump path. No signals in bare-metal.
8. **`strdup`, `strcmp`, `strlen`, `memcpy`** — libc dependencies the linker can usually resolve via newlib-nano on RTOS targets, but harder bare-metal.
9. **Stdlib classes pulled in monolithically.** Even with `--gc-sections`, the parsed-into-stdlib types (`JsonReader`, `XXHash3`, `MD5`, `ParallelDriver`) have RTTI globals that survive GC if anything reaches them transitively (e.g. via `String.hash()` → `DefaultHasher` → hash family).
10. **No bare-metal target profiles in the compiler.** `--target=` accepts arbitrary LLVM triples, but there are no preset configurations for Cortex-M / RISC-V embedded with the right `-mcpu` / `-mfpu` / linker-script combinations.

---

## Roadmap

Ten phases. Each is small enough to ship in 1–2 sessions and self-contained enough that the previous tier still works after it lands. The dependency order matters — E3 (threading) precedes E5 (init) because the runtime constructor path is what currently brings up the threading machinery; once threading is pluggable, init can be made explicit.

### E1 — `--mode=embedded-linux`

Add a CLI mode preset for Linux-class embedded (T1). Same shape as the existing `--minimal` but with embedded-friendly defaults:

```cpp
case CompilerMode::EmbeddedLinux:
    f.bounds             = BoundsCheck::Off;     // user opt-in via --bounds=on
    f.nullChecks         = NullChecks::Off;
    f.sourceTags         = false;
    f.poisonFree         = false;
    f.liveSet            = LiveSet::Bounded;     // fixed 256-entry; not Off
    f.dropChainValidate  = false;
    f.ubTraps            = false;
    f.useAfterMoveRt     = false;
    f.overflowChecks     = OverflowChecks::Wrapping;
    f.stackTraceCapture  = false;
    f.diagHints          = false;
    f.profileCounters    = false;
    break;
```

Differs from `--minimal` in that `LiveSet::Bounded` instead of `LiveSet::Off` — auto-drop discipline still works for the bounded set of allocations that fit; explicit ownership is still possible for the rest. Suitable for the "Raspberry Pi running my IoT controller" use case where you want feature-rich code but small binaries.

**Estimated effort:** half a session. Mostly a `CompilerMode.h` edit plus a CLI alias.
**Output:** HelloWorld around 25 KB (smaller than 36 KB because the existing bounded-live-set path can drop more under `--gc-sections`).

### E2 — Configurable live-set

`CAJETA_LIVE_SET_CAPACITY` is currently a `#define` in the C runtime. The 512 KB BSS is hard-coded. The fix:

1. **Compile-time configurable.** Honor `-DCAJETA_LIVE_SET_CAPACITY=N` at runtime-bitcode compile time. The runtime stays compiled into the compiler binary so this is a per-compiler-build choice today; the user changes it via `CAJETA_LIVE_SET_CAPACITY=256 ./setup.sh` and rebuilds. Easy first cut.
2. **Runtime-configurable via a flag**, mid-term. Move from a global fixed-size array to an arena pointer set at init time. `__cajeta_runtime_init` allocates the table; user code can call `__cajeta_live_set_resize(n)` before any allocations.
3. **Replace with a free-list**, long-term. The live-set is a hash table for O(1) "is this address live?" queries. For embedded, an intrusive free-list on each allocation (one extra word per object) gives the same answer in O(1) without any global table.

E2's first cut shrinks T1 BSS from 512 KB to 2 KB (256 entries × 8) with no behavior change. The free-list replacement is a deeper refactor — it eliminates the table entirely and adds one word of per-allocation overhead.

**Estimated effort:** first cut half a session; full refactor two to three.
**Output:** T1 RAM from 525 KB to ~3 KB BSS. Critical for T3/T4.

### E3 — Threading abstraction layer

Today the runtime calls `pthread_mutex_lock`, `pthread_cond_wait`, `pthread_self`, `pthread_create` directly. Replace each with a function-pointer table indirection:

```c
struct cajeta_threading_ops {
    void  (*mutex_init)(void* mu);
    void  (*mutex_lock)(void* mu);
    void  (*mutex_unlock)(void* mu);
    void  (*mutex_destroy)(void* mu);
    void  (*cond_init)(void* cv);
    void  (*cond_wait)(void* cv, void* mu);
    void  (*cond_signal)(void* cv);
    void  (*cond_broadcast)(void* cv);
    int   (*spawn)(void* (*fn)(void*), void* arg);
    void  (*yield)(void);
    /* + a few more for fibers / scopes */
};

extern struct cajeta_threading_ops* __cajeta_threading;
```

Ship three implementations, selected at link time:

- `cajeta_threading_pthread.o` — wraps libc pthread (current behavior, default for T1).
- `cajeta_threading_freertos.o` — wraps FreeRTOS `xSemaphoreTake` / `xTaskCreate` / etc. (default for T2 / Zephyr-targeted T3).
- `cajeta_threading_single.o` — all no-ops; locks become null operations; spawn fails. Suitable for single-threaded bare-metal.

The user picks one via `--target` or by linking the appropriate `.o`. The compiler's `--emit=exe` link list grows by one mandatory entry.

**Estimated effort:** one to two sessions. Mostly mechanical refactor of the runtime.
**Output:** unlocks T2 (RTOS) and T3 (bare-metal single-thread).

### E4 — Allocator abstraction

Same shape as E3, for `malloc` / `free` / `calloc`:

```c
struct cajeta_alloc_ops {
    void* (*malloc)(size_t bytes);
    void* (*calloc)(size_t count, size_t size);
    void* (*realloc)(void* p, size_t bytes);
    void  (*free)(void* p);
};

extern struct cajeta_alloc_ops* __cajeta_alloc_ops;
```

Default to libc. Ship adapters for:

- **TLSF** — two-level segregated fit, good for hard-real-time. Predictable O(1).
- **FreeRTOS `pvPortMalloc`** — uses FreeRTOS's heap_4/heap_5 algorithm.
- **Arena / bump** — user supplies a `static uint8_t arena[N]` and a pointer; allocate by advancing; free is a no-op (or arena-reset).
- **No-op** — `__cajeta_alloc` returns NULL; suitable for "stack-only" embedded code where every allocation is rejected and the static analyzer ensures heap discipline.

**Estimated effort:** one session.
**Output:** unlocks integration with embedded heaps.

### E5 — Explicit `__cajeta_init` (no constructor)

Today:

```c
__attribute__((constructor))
static void __cajeta_runtime_init(void) { ... }
```

The constructor runs before `main()` via the ELF `.init_array` mechanism, which depends on either glibc or a bare-metal startup file that honors it. Many MCU toolchains' startup files (e.g. STM32 default) don't run `.init_array` constructors.

Replace with explicit:

```c
// User-side (cajeta program):
public static int32 main(String[] args) {
    System.init();   // calls __cajeta_init under the hood
    ...
}

// Or compiler emits the call automatically when --emit=exe / --target=embedded.
```

The compiler's C-main shim (`emitCMainShim`) already wraps user entry — extend it to emit a `__cajeta_init()` call as the first statement. For non-shim entry points (user provides their own `int main`), the user inserts the call.

**Estimated effort:** half a session.
**Output:** removes the `.init_array` dependency. Required for bare-metal targets.

### E6 — Stdlib feature gating

The stdlib is parsed monolithically — every `.cajeta` file under `runtime/src/cajeta/` is in the resulting `.o`, RTTI metadata and all. With `--gc-sections`, classes that nothing references can be GC'd, but anything reachable through the implicit-Object methods (`hash()`, `toString()`, `clone()`) drags in `DefaultHasher` and friends.

The fix lands on the cajeta side:

1. **`@Optional` annotation** marks a stdlib class as eligible for stripping when not referenced. The compiler skips its codegen entirely if the parse pass detects no use site.
2. **Feature flags** — `--features=-json,-hash-xxh3,-parallel-streams` excludes whole subtrees from the stdlib parse. The flag controls which `.cajeta` files get included in `parseStdlibInto`.
3. **Defaults per mode** — `--minimal` and `--mode=embedded-*` enable a curated subset; the rest is opt-in.

A worked example: HelloWorld doesn't need `JsonReader`, `XXHash3`, `MD5`, `SipHash`, `ParallelDriver`, `HashMap` / `HashSet` / `LinkedList`, `ArrayList`'s parallel stream surface, `Pair`'s structural-hash codepath, or the file-I/O streams. Excluding all of them at parse time, plus `--gc-sections`, would shrink HelloWorld to ~12 KB (educated guess; needs validation).

**Estimated effort:** two sessions. The `@Optional` machinery is straightforward; the per-feature flag table is mechanical; the use-detection pass needs careful design to avoid false positives.
**Output:** another 50–75% off the binary size for programs that touch a narrow stdlib slice.

### E7 — Libc-free runtime

For T3/T4 bare-metal, libc may be absent. The runtime currently uses `strdup`, `strcmp`, `strlen`, `memcpy`, `memset`, `fprintf`, `abort`, `exit`, `malloc`, `free`. Replace each with either:

- A built-in (`__builtin_memcpy`, `__builtin_strlen`) — clang lowers these to direct loop / movsq instructions when libc isn't linked.
- An internal cajeta-runtime version (`__cajeta_internal_strcmp` etc.) for the cases the builtin doesn't cover.
- A no-op or panic for things that don't make sense bare-metal (`exit` becomes `__cajeta_panic`).

**Estimated effort:** one session.
**Output:** unlocks "no libc" link line.

### E8 — Pluggable I/O

`__cajeta_emit(stream, bytes, count)` is the runtime function every `print` / `println` ultimately routes through. Currently it calls `write(fd, bytes, count)` via libc.

Make it pluggable:

```c
typedef void (*cajeta_emit_fn)(int32_t stream, const void* bytes, size_t count);
extern cajeta_emit_fn __cajeta_emit;

// Default for T1:
static void emit_posix(int32_t fd, const void* bytes, size_t n) { write(fd, bytes, n); }

// Example user adapter for ARM Semihosting:
static void emit_semihosting(int32_t fd, const void* bytes, size_t n) {
    semihost_write(fd, bytes, n);
}
```

User sets `__cajeta_emit = emit_uart` at startup (after E5's explicit init). Now `System.stdout.println("hello")` goes to UART instead of stdout.

**Estimated effort:** half a session.
**Output:** any I/O backend the user wants. Critical for bare-metal where there is no stdout.

### E9 — Bare-metal target profiles

Add `--target=cortex-m4`, `--target=cortex-m0+`, `--target=riscv-rv32imac` etc. preset profiles that:

- Set the LLVM triple correctly (`thumbv7em-none-eabihf` etc.).
- Enable `--mode=embedded-bare` (a new mode built on top of E1's `embedded-linux` with extra trims).
- Auto-link the relevant threading-ops / alloc-ops / emit adapters.
- Emit `.text.startup` linker hints so the linker script can place `__cajeta_init` at boot.
- Document the linker-script integration (the user still provides their own linker script; cajeta emits no canned ones for v1).

**Estimated effort:** two sessions, mostly investigation + spec work on the specific MCU profiles.
**Output:** out-of-box "cajeta hello world for Cortex-M4F" with `cajeta --target=cortex-m4 --emit=obj ...` producing a link-ready `.o`.

### E10 — Heapless mode

The aspirational tier T4 target. Make heap allocation an opt-in:

- `--heap=off` rejects every `heap T(...)` expression at compile time.
- Stack and views remain — every owned value lives on the frame or in a fixed-size pool.
- The borrow checker already handles stack-only lifetimes; this is mostly about restricting the surface.

The static analyzer needs to ensure that types like `ArrayList`, `HashMap`, `HashSet` aren't used directly (they internally allocate). Either gate them at compile time, or provide stack-only variants (`StackArray<T, N>` with compile-time-fixed N).

**Estimated effort:** three to four sessions. Wide-spectrum impact; needs careful design for the no-heap stdlib subset.
**Output:** unlocks T4 (sub-32 KB flash). May fundamentally split the stdlib into "heap" and "heap-free" subsets.

---

## Lean linking & the reflection keep-set (shipped)

Reflection forces every class to register itself into `llvm.global_ctors` so it stays discoverable by name. That registration anchor defeats `--gc-sections`, so a naïve build links **every** class — and the whole reflection/TLS/OpenSSL tail — into even a one-line program. The lean linker strips a class's registration anchor when no reflection site can reach it, letting section-GC drop it.

**`--emit=exe` defaults to `--link-mode=lean`.** `--link-mode=full` (alias `--keep-all`) opts out and keeps every class (the old behavior; still the default for `--emit=ir|obj|cja|uber` and JIT/test). Measured on HelloWorld:

| | `--link-mode=full` | `--link-mode=lean` (default) |
|---|---|---|
| binary | 2.51 MB | **370 KB** (−85%) |
| reflection reg-ctors | 268 | 0 |
| OpenSSL symbols | 45 | 0 |

**Sound by construction via bounded reflection.** The keep-set is a *reflection-discoverability* set, not a code-liveness set (`--gc-sections` already keeps any class normal code references — an unkept class merely stops being reflectively *findable*). Each reflection site contributes exactly its statically-resolvable blast radius:

| Site | Keeps |
|---|---|
| `Class.subtypes<Base>()`, `Class.heapInstance<Base>(name)` | `Base`'s closed-world subtype closure (from the type hierarchy, so rare-conditional leaves are safe) |
| `Class.forName("literal")` | that one class |
| `Class.classesInPackage("pkg")` | classes in `pkg` |
| `Class.classesAnnotated<@A>()` / `("A")` | classes bearing `@A` |
| `@Retained` | unconditional manual keep-pin |

Because a bounded site can only resolve to a class in its closure, and the closure is kept, there is no "stripped but reached" failure — no trace, no manifest, no completeness proof. **Unbounded** reflection (`forName(dynamicString)`, `allClasses()`, a top-type bound, `TemplateArgument.getType()`) can't be narrowed, so it degrades to a conservative **keep-all + warning** — never a silent strip. So a default-lean build cannot crash on an unexercised reflective path.

**Tuning workflow.** The keep-set is generated, never authored — there is no include/exclude file. To shrink a binary, tighten the bound (or drop a class from the classpath). Three diagnostics make the keep-set legible:

- `--why-kept=<canonical.Class>` — prints the site/root that kept a class (`subtype closure of …`, `forName("…")`, `@Retained keep-pin`, …) or `STRIPPED`.
- `--keepset-json=<path>` — writes the full keep-set with per-class provenance (`{class, keptBy}`); `forcesAll:true` when a build went unbounded.
- `warning: [reflection-forces-keep-all] <site> — …` — emitted per unbounded site, naming the selector to tighten.

```
$ cajeta --emit=exe --why-kept=app.PluginA --keepset-json=ks.json \
         app.Main.run src/ out/
why-kept: app.PluginA — kept by subtype closure of app.Plugin
```

This is the Java differentiator: erased `Class<T>` + open world means even GraalVM/R8 need a hand-written reflection config; cajeta's reified monomorphized types + closed-world link make the bound a *complete* keep-set automatically. (Tier-1 method-level tree-shaking and Tier-2 `internalize`+`globalDCE` build further on top; see `plans/compiler/lean-linker-dce.md`.)

---

## Per-tier size targets

Goals after each roadmap phase lands. Hello-world program (single `println`), x86_64 baseline for T1 / T2, ARM Cortex-M for T3 / T4. Actual values will depend on the host toolchain and final phase decisions.

| Tier | Target binary | Target RAM (BSS + initial stack) |
|---|---|---|
| **T1 (today)** | 36 KB | 525 KB (live-set dominates) |
| **T1 (after E1 + E2)** | 25 KB | 3 KB |
| **T1 (after E6)** | 12 KB | 3 KB |
| **T2** | 30 KB | 8 KB |
| **T3** | 20 KB | 4 KB |
| **T4** | < 8 KB | < 2 KB |

The T1-after-E6 target of 12 KB matches dynamic-linked C with `printf` (~16 KB) and beats C++ `std::cout`. T3 at 20 KB sits with Zig (~10–20 KB embedded). T4 < 8 KB enters Zig / no_std Rust territory.

---

## Comparisons

Where cajeta could be after the full roadmap:

| Language | T1 (Linux) | T2 (RTOS) | T3 (bare-metal MCU) | T4 (deep embedded) |
|---|---|---|---|---|
| C with libc | 16 KB | 8 KB | 4 KB | < 1 KB |
| Zig | 5 KB | 5 KB | 5 KB | 1 KB |
| Rust no_std | n/a | 15 KB | 10 KB | 4 KB |
| C++ no-exceptions / no-rtti | 25 KB | 20 KB | 15 KB | n/a |
| **Cajeta (roadmap)** | **12 KB** | **30 KB** | **20 KB** | **< 8 KB** |

Cajeta sits a bit larger than Zig in every tier — the price of exceptions, vtables, drop chain, and structured concurrency. Smaller than Rust no_std at most tiers because cajeta doesn't carry a borrow-checker-induced monomorphization explosion. Comparable to C++ no-exceptions / no-rtti but with memory safety and a friendlier surface.

---

## Open questions

Items that need a design pass before the related phase ships:

1. **ISR safety.** Cajeta's drop chain mutates a per-thread linked list at every owner declaration / scope exit. Inside an ISR, drop-chain manipulation is unsafe (the carrier thread might be mid-mutation when the ISR fires). Options: (a) compile ISR functions with `@NoDrop` annotation that disables drop-chain wiring; (b) declare ISRs as `static void isr() @ISR` and have the compiler emit a "save+restore drop top" prologue/epilogue; (c) reject heap / owned allocations inside ISR bodies entirely. v1 should probably be (c) plus narrow @ISR annotation.
2. **Custom panic / abort.** Today the runtime calls libc `abort()` directly at its fatal-error sites (allocation failure, drop-chain violations, etc.), which triggers SIGABRT, the dump handler, and exit. (There is no single `__cajeta_panic` chokepoint yet — E7 proposes introducing one.) Bare-metal wants something different — typically a debug-trap (`BKPT`), a soft reset, or a watchdog-controlled hang. Need pluggable.
3. **Floating-point exclusion.** Many MCUs have no FPU (Cortex-M0, M0+, M23). A `--no-fp` flag would reject `float*` types at compile time. Today's stdlib has `float64`-typed APIs throughout (`Math.sin`, `Math.sqrt`, stream `reduce(double, ...)`); they'd need to be feature-gated alongside the no-fp flag.
4. **Compile-time evaluation.** Embedded code wants `const`-time computed lookup tables. Cajeta doesn't yet have a `comptime { ... }` block in Zig's spirit. Worth designing alongside the embedded work since LUTs are pervasive in firmware.
5. **DMA and zero-copy.** Cajeta's `view` types already do zero-copy over byte slices. For DMA buffer ownership across an ISR/main-thread boundary, the borrow checker needs an "ownership transfer to hardware peripheral, returned after completion" idiom. Likely an annotation on the type that suppresses normal borrow rules for DMA-aware code paths.
6. **Linker scripts.** Cajeta won't ship canned linker scripts (too many MCU variants), but the compiler should produce well-named sections (`.cajeta.startup`, `.cajeta.rtti`, etc.) so user scripts can place them precisely.

---

## Status

| Phase | Status | Notes |
|---|---|---|
| Baseline (T1) | **shipped** | HelloWorld 36 KB; full feature surface |
| Lean linker (DCE Tier-0) | **shipped** | `--link-mode=lean` default for `--emit=exe`; bounded reflection keep-set; `--why-kept` / `--keepset-json` / forces-keep-all warning. HelloWorld 2.51 MB→370 KB |
| Clinit-level DCE (Tier 1.5) | designed | Strip `llvm.global_ctors` registration for provably-dead classes' static initializers. Whole-program external-purity + dead-static analysis; depends on Tier-1 RTA. **Naive `keepsClass`/static-only gate reintroduces task #68 (`@Logged` reflection-only `log` → null) — see `plans/compiler/lean-linker-dce.md` §3.6.** |
| E1 — `--mode=embedded-linux` | proposed | Half a session; mostly `CompilerMode.h` |
| E2 — Configurable live-set | proposed | First cut easy; free-list refactor multi-session |
| E3 — Threading abstraction | proposed | One to two sessions |
| E4 — Allocator abstraction | proposed | One session |
| E5 — Explicit `__cajeta_init` | proposed | Half a session |
| E6 — Stdlib feature gating | proposed | Two sessions |
| E7 — Libc-free runtime | proposed | One session |
| E8 — Pluggable I/O | proposed | Half a session |
| E9 — Bare-metal target profiles | proposed | Two sessions per architecture family |
| E10 — Heapless mode | aspirational | Three to four sessions; design-heavy |

Total time to unlock T2 (RTOS): E1 → E3 → E4 → E5 → E6, roughly five to six sessions.
Total time to unlock T3 (bare-metal MCU): the above plus E7 → E8 → E9, another three to four sessions.
Total time to T4 (deep embedded): the above plus E10, another three to four sessions.

The first ship (E1) lands real value immediately — a 25 KB HelloWorld on Linux SBCs is competitive with C++ — and each subsequent phase opens a strict superset of targets. No phase regresses prior tiers.
