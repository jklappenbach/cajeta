# Per-call cost of small methods (spec)

File: tensor-element-access. Status: draft, 2026-10-19. Opened 2026-10-17 by
xgboost-data-prep 1.2.3 as a Tensor question; widened on Julian's word
2026-10-19 to the compiler costs the measurement found, line frames included.

## 1. Definition

### 1.1 Problem

`Tensor<T>.flatGet(i)` costs 16 ns a read where a plain array read costs 2.3
ns (O0) or 0.5 ns (O2). Most of that cost is not in Tensor. It comes from
three things the compiler does to every method of the same shape, so any
stdlib or library method that guards and throws pays it, and every call pays
the third.

### 1.2 Measurement

Probe `probe.Tea` on proton (Strix Halo cpu), cajeta v0.43.1, 2026-10-19:
20M reads of a 1M-element `Tensor<float64>`, ns per element, median of 3.

| build | flatGet | flatSet | array | hostBuffer | decodeI32 2^23 |
|---|---|---|---|---|---|
| O0, debug line (default) | 16.0 | 17.3 | 2.35 | 2.35 | 147 ms |
| O2 | 15.1 | 15.3 | 0.49 | 0.49 | 125 ms |
| O2, full LTO | 15.4 | 16.0 | 0.49 | 0.49 | 127 ms |
| O2, debug off | 11.6 | 13.1 | 0.49 | 0.49 | 106 ms |
| O2, debug off, --lazy-scope | 6.6 | 8.0 | 0.49 | 0.48 | 65 ms |

From the IR (`--emit=ir`), per read:

- **1.2.1 A scope frame per call.** `Storage.get` contains a `throw`, and
  `Method::bodyNeedsScopeFrame` treats a throw as a spawn site
  (`Method.cpp:95`). The method then calls `__cajeta_scope_save_top`,
  `__cajeta_scope_enter` (a `malloc`) and `__cajeta_scope_exit_to` (a
  `free`) on every call, though the throw is never taken. About 5 ns. The
  throw has been in that predicate since frame elision landed (50c3926e,
  2026-06-21) with no recorded reason.
- **1.2.2 A runtime vtable lookup.** `flatGet` calls `store.get` through
  `__cajeta_vtable_lookup`, a binary search over the vtable by method hash,
  then an indirect call. Nothing overrides `Storage.get`, but the call can
  never inline, even with full LTO. Most of the remaining 6 ns.
- **1.2.3 Line frames.** `--debug-info=line` (the default, release builds
  included) adds `__cajeta_line_enter`/`leave` per call, about 3.5 ns. Each
  is an out-of-line C call (`cajeta_rt_core.c:313`) that finds the running
  context's shadow stack (`__cajeta_shadow_ptr`), stores a descriptor and
  moves an index. The work is a few instructions; the cost is the calls,
  which the optimizer cannot see through. Once 1.2.1 and 1.2.2 are fixed,
  this is the largest per-call cost left.
- **1.2.4 Npy decode.** `Npy.decodeI32` and its siblings call `flatSet` once
  per element. 147 ms for SfuMath's 32 MB capture is most of cajeta-xgboost's
  cold first fit with the captures (xgboost-device-followups 3.2.3).

### 1.3 Scope

The compiler (scope-frame placement, call lowering, line-frame lowering), the
runtime's shadow stack, and the stdlib
(`cajeta.math.Tensor`, `Storage`, `cajeta.math.npio.Npy`). Ships in the next
toolchain release.

### 1.4 Non-goals

- Changing what `--debug-info=line` records, or the default `--opt`. The
  frames get cheaper; the stack traces stay the same.
- A new bulk-access API. `hostBuffer()`/`hostBase()` already exist (v0.22).
- `--lto=thin`, which fails to link on v0.43.1 ("Invalid summary version
  13"). That is its own defect (8.5).

## 2. Throwing methods

A method that can throw but does not spawn, scope or detach costs no
allocation on the path that does not throw.

- **2.1** When a method's body throws but never spawns, a call that does not
  throw allocates nothing for structured concurrency.
- **2.2** When such a method does throw, the exception reaches the caller
  with the same type, message and stack trace as today.
- **2.3** When a method spawns, detaches or opens a `scope`, its child tasks
  are still joined on every exit path, including a throw.
- **2.4** When a throw unwinds through a method that has a scope frame of its
  own, that frame is still waited and popped.

## 3. Calls to methods nothing overrides

A virtual call is a direct call when the program has a single implementation
of the method for the receiver's static type.

- **3.1** When a class method has no override anywhere in the linked program,
  a call to it is direct and the optimizer can inline it.
- **3.2** When an override exists, the call dispatches to it, as today.
- **3.3** When an interface or abstract method is called, dispatch is
  unchanged.
- **3.4** When the program is built from several `.cja` archives, a method
  overridden in any of them is not called directly.
- **3.5** When a `--release` build without LTO calls a small stdlib method
  directly, the optimizer can inline it into the caller.

## 4. Line frames

A line frame costs no runtime call, and records what it records today.

- **4.1** When a method with a line frame is entered and left, the push and
  pop are inline code, not calls into the runtime.
- **4.2** When an exception is thrown or a program faults, its stack trace
  shows the same frames and lines as today.
- **4.3** When a fiber suspends on one carrier and resumes on another, its
  frames stay on its own shadow stack.
- **4.4** When the profiler samples a running thread or fiber, it reads the
  same stack it reads today.
- **4.5** When the stack is deeper than the shadow stack's capacity, push and
  pop stay balanced, as today.
- **4.6** When a small method is inlined into its caller, its frame is still
  pushed, so traces do not lose it.

## 5. Tensor element access

- **5.1** When a contiguous host Tensor is read with `flatGet` or written with
  `flatSet` in a release build, it costs within the target of 8.3 against a
  plain array.
- **5.2** When the data is device-resident, `flatGet` and `flatSet` still
  throw `PlacementException` with today's message.

## 6. Npy decode

- **6.1** When `Npy` decodes a numeric `.npy` image of a fixed-width
  little-endian type, it copies the data in bulk, not one element at a time.
- **6.2** When the image is big-endian, Fortran-order or a type Npy does not
  read today, the result or the error is unchanged.
- **6.3** When SfuMath loads a capture table, the load costs within the target
  of 8.3.

## 7. Verification

- **7.1** When the change lands, golden-IR tests show which method shapes keep
  a scope frame, which call sites are direct and how a line frame lowers,
  and tests assert both that each change fires and that it does not fire
  where it must not.
- **7.2** When the concurrency, exception, ownership, fiber and profiler
  suites run, they are green with the change, on Linux and Windows.
- **7.3** When cajeta-xgboost and cajeta-json are rebuilt on the new
  toolchain, their results are unchanged, and their timings are recorded.

## 8. Open questions

All five were decided by Julian on 2026-10-19, each as recommended; 8.3
was then tightened (see there).

- **8.1 How a throwing method avoids the frame.**
  - (a) Drop `throw` from the scope-frame predicate, after showing a throw
    needs no frame of its own (2.2, 2.4).
  - (b) Make `--lazy-scope` the default, which pushes the frame at the spawn
    site for every method.
  - Recommended: (a). It is the narrow change the measurement supports. (b)
    changes every spawning method and has been opt-in since June. Whether
    `try`, `switch`, `synchronized` and `yield` need their frames too is
    checked in the same unit; any that do not are dropped with `throw`.
- **8.2 How a call becomes direct.**
  - (a) Class-hierarchy analysis when the executable is built: every class
    is known then, so a method with no override is called directly.
  - (b) Mark `Storage` and `Tensor` accessors `final` in the stdlib.
  - Recommended: (a). (b) fixes two classes and leaves every other library
    paying. If (a) cannot see the whole program in some build mode, that mode
    keeps today's dispatch.
- **8.3 Targets.** On proton, cajeta's own toolchain:
  - `--release` (O2, debug line, no LTO): `flatGet` and `flatSet` at most
    1.5 ns, from 15;
  - O2 with debug info off: at most 1 ns, which shows the call inlined;
  - `Npy.decodeI32` of 2^23 elements at most 10 ms, from 125. A 32 MB copy
    is about 3 ms.
  Julian first accepted 5 ns and 20 ms, then asked whether that settled for
  less. It did: 5 ns kept the line-frame calls (1.2.3) and a 7-10x gap to an
  array. Tightened with section 4 added, 2026-10-19.
- **8.4 Which Npy types.** Proposed: every fixed-width type Npy reads today
  (int8 to int64, float16 to float64, bfloat16, bool).
- **8.5 Thin LTO.** Proposed: file `--lto=thin`'s link failure as its own
  defect spec, outside this one.
- **8.6 Inlining stdlib methods without LTO.** `--release` measured the
  same as plain O2 (15 ns, 2026-10-19): the stdlib is its own object file
  and `--release` has no LTO, so a direct call still cannot inline.
  - (a) Give the user module a discardable copy of each small stdlib method
    body it calls, for the optimizer to inline.
  - (b) `--release` implies `--lto=full`.
  - (c) Meet the targets under `--lto=full` only.
  - Decided by Julian 2026-10-19: (a).
