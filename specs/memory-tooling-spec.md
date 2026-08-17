# memory-tooling — cross-platform memory correctness for Cajeta (spec)

> Status: **draft, pending approval**. Authored with the **design** skill.
> The actionable *how* lives in `agents/memory-tooling-plan.md`.
>
> Supersedes the `valgrind-interop` draft (2026-08-17, never approved). That
> draft made Valgrind primary, which fragments the story: Valgrind has no Windows
> port and no Apple Silicon port, so it can never be the answer for a language
> targeting all three. AddressSanitizer can, and is (§1.6).
>
> Sibling to [`cajeta-profiler`](cajeta-profiler-spec.md) and deliberately
> separate: that capability answers *where does time go*, this one answers *is the
> memory correct*. Neither gates the other.

---

## 1. Definition

### 1.1 Purpose
Give Cajeta developers memory-correctness diagnostics — use-after-free, buffer
overflow, use-after-scope, leaks — **on every platform Cajeta targets**, through
one mechanism, with results browsable in the IDE.

### 1.2 The problem it solves
Cajeta has strong compiled-in checking (`poisonFree`, `liveSet`,
`dropChainValidate`, `useAfterMoveRt`, bounds and null checks, `ubTraps`), and
that machinery covers Cajeta-level code well. It does not cover the part of the
system written in C: roughly forty files of native runtime, the `@Native` FFI
boundary, and vendored dependencies. That is where memory bugs actually live —
and where the one recorded instance came from, a freed-lock caught by an ad-hoc
Valgrind run and noted in `test/parser/AsyncSyntaxTests.cpp`.

Three things currently prevent any external memory tool from working well here:

1. **No DWARF.** The compiler emits no debug sections at all, so every frame is a
   mangled name with no file or line.
2. **Fibers.** The runtime switches stacks — `swapcontext` on POSIX, Win32 Fibers
   on Windows — and tells no tool about it, so stack traces across a yield are
   wrong and spurious errors appear.
3. **The frame bump arena.** One large reservation, objects bump-allocated,
   reclaimed by resetting a pointer. Any tool sees a single allocation, so
   use-after-scope inside the arena is undetectable.

### 1.3 Scope

**Phase 1 — make the tooling work.** Self-contained; delivers working diagnostics
from the command line and in CI on all platforms, with no IDE work.

1. **DWARF emission** behind a compiler flag.
2. **Sanitizer build support** — actually implementing the declared flavor keys.
3. **Fiber stack annotation** so stack switches are tracked.
4. **Arena annotation** so scope reclamation is visible.
5. **Suppressions** for known-benign patterns.
6. **Build-tool integration** and a **CI leg** on every platform.

**Phase 2 — surface results in the IDE.** Depends on Phase 1; nothing in Phase 1
depends on it.

7. **A report view** — browse a completed run's findings, navigate to source.
8. **A live view** — findings as they occur during a run or debug session.

### 1.4 Non-goals
1. **A custom Valgrind tool.** Valgrind's tool API is compile-time — a tool ships
   as a patched Valgrind binary. More to the point, Cajeta's own compiled-in
   checks already cover most of what such a tool would rediscover.
2. **Replacing the language's own safety checks.** This targets the part the
   language cannot check.
3. **MemorySanitizer.** It is Linux-only and requires every dependency including
   libc to be instrumented. Its one unique capability — uninitialized reads — is
   covered on Linux by Valgrind as a supplement (§4).
4. **ThreadSanitizer** in v1. The `tsan` flavor key stays unimplemented; fibers
   confuse TSan the same way they confuse every stack-switching-unaware tool, and
   the annotation work is a separate exercise.
5. **Full DWARF type information** in v1 (§2).

### 1.5 Constraints
1. **Annotations must be free when the sanitizer is not active.** ASan's fiber and
   poisoning APIs compile to nothing when not building with `-fsanitize=address`.
2. **DWARF must be opt-in.** It is large, and the default build deliberately
   carries no debug sections.
3. **DWARF does not replace the line-info shadow stack.** The shadow stack works
   in the JIT, in AOT binaries, and on device targets, none of which carry debug
   sections. Both stay.
4. **The IDE plugin targets IntelliJ IDEA Community.** CLion has sanitizer and
   Valgrind integration; IDEA Community does not, so neither can be relied on.

### 1.6 Why AddressSanitizer is the primary mechanism

| | AddressSanitizer | Valgrind Memcheck |
|---|---|---|
| Linux | yes | yes |
| **Windows** | **yes** | **never — no port has ever existed** |
| **macOS Apple Silicon** | **yes** | **no arm64 port** |
| macOS Intel | yes | port exists but trails releases badly |
| Android / iOS | yes | Android only |
| Slowdown | ~2× | ~20–50× |
| Stack and global overflows | yes | no |
| Uninitialized reads | no | **yes** |
| Needs recompilation | yes | no |

ASan is the only mechanism that covers the full target matrix, and it is an LLVM
pass — which Cajeta, being LLVM-based, can enable directly. The declared
`asan`/`ubsan` flavor keys already exist with no implementation behind them.

**Valgrind remains available as an optional Linux supplement** for the one gap
that matters — uninitialized reads in native C (§4). It is not a second primary
path, and the project is complete without it.

---

## 2. DWARF emission

The single highest-leverage item, because it serves sanitizer symbolization, gdb,
lldb, perf, and crash dumps at once. Scope for v1 is **line tables and function
scopes** — enough for a correct, navigable backtrace. Variables and types are
deferred (§10.2).

- **2.1** When a program is built with the DWARF flag, its binary carries standard
  debug sections that any DWARF consumer reads.
- **2.2** When the flag is absent, no debug sections are emitted and binary size
  and build time are unchanged.
- **2.3** When a sanitizer reports a finding, each Cajeta frame resolves to a
  function name, source file, and line.
- **2.4** When a method is inlined, the DWARF describes the inlining so a
  backtrace still attributes to the original source location.
- **2.5** When DWARF is emitted, the line-info shadow stack works unchanged, and
  neither mechanism depends on the other.
- **2.6** When source paths must be reproducible, the existing debug-prefix-map
  and source-date-epoch options apply to the emitted DWARF.
- **2.7** When DWARF is emitted for a JIT-compiled unit, it is registered with the
  standard JIT debug interface.
- **2.8** When the flag is set, the compile cache key changes, so a DWARF and a
  non-DWARF build never alias.
- **2.9** When a Cajeta symbol appears in a demangling tool, it is either demangled
  correctly or left legible — never rendered as corrupted text.
- **2.10** When DWARF is emitted on Windows, the platform's expected debug format
  is produced so native tools can consume it.

## 3. Sanitizer builds

The flavor keys exist; nothing implements them. This makes them real.

- **3.1** When a build requests the address sanitizer, the program is instrumented
  and linked against the sanitizer runtime.
- **3.2** When it is not requested, no instrumentation is applied and there is no
  residual cost.
- **3.3** When a sanitizer build runs on Linux, Windows, or macOS including Apple
  Silicon, it works the same way and reports findings in the same form.
- **3.4** When a finding occurs, the program reports it with a symbolized stack
  and — under the default configuration — halts, so the first error is the one
  investigated.
- **3.5** When a sanitizer build is requested, DWARF is emitted automatically,
  because an unsymbolized report is not actionable.
- **3.6** When the undefined-behavior sanitizer is requested, it composes with the
  address sanitizer in one build.
- **3.7** When a sanitizer flavor is selected, the compile cache key reflects it.
- **3.8** When a leak check is available on the platform, leaks are reported;
  when it is not, the build states that rather than silently omitting them.
- **3.9** When a sanitizer build is run through the build tool, its runtime options
  and suppressions are applied without being named explicitly.

## 4. The two gaps, and how they are closed

ASan is not a superset of Memcheck. It catches **more** than Memcheck on stack
and global overflows, use-after-return, and use-after-scope; it catches **less**
on two counts. Both gaps are addressed without fragmenting the primary path.

### 4.1 Uninitialized reads
No tool closes this cross-platform — MemorySanitizer is Linux-only and requires
instrumenting every dependency including libc, so it is no more portable than
Valgrind. The gap is narrower here than in a typical C project by construction:
the runtime allocator is `calloc`-based and zero-initialized, so main-allocator
heap objects cannot be uninitialized. The exposure is the deliberately
uninitialized allocation variant, the bump arena, and stack locals in native C.

Three measures, in order of coverage:

- **4.1.1** When any code is built, automatic variables are initialized to a
  poison pattern, so an uninitialized read is deterministic and visibly wrong
  rather than silently plausible. This applies on every platform at negligible
  cost.
- **4.1.2** When native C is compiled, uninitialized-use warnings are enabled and
  treated as errors, catching a share of the class before runtime.
- **4.1.3** When exhaustive detection is wanted, Valgrind on Linux reports the
  remainder (§4.3).

### 4.2 Leak detection on Windows
LeakSanitizer is not available on Windows. Cajeta's own `liveSet` already tracks
Cajeta-level allocations cross-platform and categorizes them better than LSan
could, so the uncovered surface is C-runtime leaks on Windows specifically.

- **4.2.1** When a leak check runs on a platform providing one, leaks are reported.
- **4.2.2** When the platform provides none, the build states that rather than
  silently omitting leak results.
- **4.2.3** When Cajeta-level leak accounting is available, it is reported on every
  platform regardless of sanitizer support.

### 4.3 Valgrind as an optional Linux supplement
Valgrind covers uninitialized reads and analyzes binaries without recompilation —
useful for vendored C. It is Linux-only, and the capability is complete without it.

- **4.3.1** When Valgrind is run on Linux against a Cajeta binary, fiber stack
  switches are tracked and stack traces are correct.
- **4.3.2** When Valgrind runs, arena scope reclamation is visible to it.
- **4.3.3** When Valgrind is unavailable or the platform is not Linux, nothing in
  this capability is blocked.
- **4.3.4** When a suppression is needed for Valgrind, it lives alongside the
  sanitizer suppressions and is maintained the same way.

## 5. Fiber annotation

One requirement, two implementations, because the runtime has two fiber backends.

- **5.1** When a fiber switch occurs, the active tool is told, so stack traces
  taken afterwards are correct for the running fiber.
- **5.2** When a fiber is created or destroyed, its stack is registered or released.
- **5.3** When a fiber program runs under a sanitizer, no spurious findings arise
  from stack switching.
- **5.4** When the runtime uses the POSIX `ucontext` backend, annotation applies.
- **5.5** When the runtime uses the Win32 Fibers backend, annotation applies
  equivalently — this is the path Valgrind could never have covered.
- **5.6** When no tool is active, annotation costs nothing.

## 6. Arena and allocator annotation

- **6.1** When an object is allocated or freed through the runtime allocator, the
  active tool sees it at the correct size.
- **6.2** When the frame arena hands out a block, it is visible as an allocation
  rather than part of one opaque reservation.
- **6.3** When an arena scope is reclaimed by resetting to a mark, every block
  above that mark becomes inaccessible, so use-after-scope is reported.
- **6.4** When nested scopes are reclaimed, each reclaims exactly its own blocks.
- **6.5** When a block is deliberately handed out uninitialized, the tool is told,
  so genuinely uninitialized reads are still caught and deliberate ones are not
  reported.
- **6.6** When poison-on-free is enabled, it does not mask or conflict with the
  tool's own detection.
- **6.7** When arena memory is live at exit, it is attributable rather than hidden.

## 7. Suppressions

- **7.1** When a known-benign pattern produces a finding, a maintained suppression
  file silences it.
- **7.2** When a suppression is added, it names the specific pattern rather than
  silencing a whole library.
- **7.3** When a new unsuppressed finding appears, it surfaces.
- **7.4** When suppressions exist for more than one tool, they are maintained
  together and applied automatically by the build tool.

## 8. Continuous integration

- **8.1** When the test suite runs under the address sanitizer in CI, a new
  unsuppressed finding fails the run.
- **8.2** When the CI leg runs, it runs on **all supported platforms**, not only
  Linux — this is the concrete payoff of choosing ASan.
- **8.3** When a failure occurs, the report is retained as an artifact.
- **8.4** When the leg runs, its cost is bounded; ASan's ~2× slowdown makes a
  broader subset affordable than a Valgrind-based leg would.
- **8.5** When the optional Valgrind supplement runs, it runs on Linux only and
  does not gate the others.

## 9. Phase 2 — surfacing results in the IDE

The plugin consumes tool output directly rather than depending on CLion's
integration, because it targets IDEA Community. Sanitizer and Valgrind reports
differ in format but share a presentation model: a finding, a kind, a stack of
frames.

### 9.1 Report view
- **9.1.1** When a report exists, it can be opened in a tool window.
- **9.1.2** When a report is open, findings are grouped by kind, with counts.
- **9.1.3** When a finding is selected, its stack is shown with each frame
  navigable to source.
- **9.1.4** When a frame is in Cajeta source, it opens the `.cajeta` file; when it
  is in the native runtime, it opens the C source.
- **9.1.5** When a finding is selected, a suppression can be generated and copied.
- **9.1.6** When a report has no findings, the view says so plainly.
- **9.1.7** When a report comes from a different supported tool, the same view
  presents it.

### 9.2 Live view
- **9.2.1** When a program runs under a tool from the IDE, findings appear as they
  occur rather than only at exit.
- **9.2.2** When a finding appears during a debug session, it is presented
  alongside the debugger's state for that session.
- **9.2.3** When a finding is reported, navigating to its source does not require
  the run to have finished.
- **9.2.4** When the process exits, accumulated findings browse identically to a
  completed report.
- **9.2.5** When findings arrive in volume, the view stays responsive, bounds what
  it retains, and reports anything dropped.
- **9.2.6** When the default halt-on-error behavior is active, the finding that
  stopped the program is the one presented.

---

## 10. Resolved decisions

- **10.1 Primary mechanism.** AddressSanitizer. It is the only option covering
  Linux, Windows, and macOS including Apple Silicon (§1.6), and it is ten to
  twenty-five times faster than Valgrind.
- **10.2 Valgrind's role.** Optional Linux supplement for uninitialized reads in
  native C (§4). Not a second primary path; the capability is complete without it.
- **10.3 Custom Valgrind tool.** No (§1.4.1).
- **10.4 MemorySanitizer.** No — Linux-only and requires instrumenting every
  dependency (§1.4.3).
- **10.5 DWARF versus the shadow stack.** Both; different consumers (§1.5.3).
- **10.6 Callgrind's relationship to `cajeta-profiler`.** Complementary. Callgrind
  gives deterministic instruction counts with no compiler work, useful for
  regression comparison, but cannot serve that spec's instrumentation tier —
  recorded there in its §3.
- **10.7 IDE route.** Read tool output in the plugin rather than depending on
  CLion (§1.5.4).

## 11. Open questions

- **11.1** Whether the DWARF flag is an independent axis or an extension of
  `--debug-info`. They are orthogonal — one drives Cajeta's own safepoint and
  shadow-stack machinery, the other standard debug sections — which argues for
  independence, but two similar-looking flags invite confusion.
- **11.2** When to add DWARF variable and type information; it overlaps the
  debugger work already in flight.
- **11.3** **Whether ASan applies to JIT-compiled code.** ASan instruments at
  compile time; for AOT this is straightforward, but the JIT would need to run the
  pass over generated modules and link the sanitizer runtime. If it cannot, the
  JIT path is uncovered and that should be stated rather than assumed.
- **11.4** Whether ThreadSanitizer is worth a later pass over the fiber scheduler,
  given it needs its own fiber annotations.
- **11.5** Whether §9.2's live view should also stop the program at a finding.
  ASan halts by default, which gives most of that behavior for free; Valgrind
  would need its gdbserver and a GDB remote client, which is a much larger lift.
