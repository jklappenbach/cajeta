# xpu-kernel-independence: new kernels and new models without compiler changes

Status: **draft**, written 2026-10-03 at Julian's request; its §7 decisions
taken the same day in an interactive review. Plan:
[`agents/xpu-kernel-independence-plan.md`](../agents/xpu-kernel-independence-plan.md).

## 1. Definition

A third party can write new kernels, a new AI framework or a new model in a
cajeta library, and build it with a released compiler. Julian's rule,
2026-10-03:

> I'm ok with needing new building blocks when new hardware is introduced.
> That happens every few years. What I'm not ok with is having to alter the
> compiler for new kernel work, or to support a new model.

So the compiler owns two things. It owns the building blocks a piece of
hardware offers, added when the hardware is new. It owns the correctness of
lowering every program it accepts. Everything above that line belongs to
libraries: kernel algorithms, quantization formats, fused epilogues, tile
shapes and model code.

### 1.1 The evidence

An audit on 2026-10-03 read cajeta-llm's 467 commits and cajeta's commits
from the same window, 2026-08-13 to 2026-10-03. The tables are in the
session's scratchpad (`audit-cajeta.tsv`, `audit-llm.tsv`). The headline
numbers below were spot-checked against the repositories.

| Measure | Count |
|---|---|
| cajeta commits driven by llm work | 183 of 1,180 |
| llm features that needed a compiler change | 44 of 96 |
| llm features that needed a workaround instead | 20 of 96 |
| llm kernels routed around on cpu because they give wrong answers | 13 |
| `@Unlowered` holds in cajeta-llm | 24 |

Weekly counts of llm-driven compiler commits were 30 in the week of
2026-08-17 and 42 in the week of 2026-09-21, so the dependence is not
falling. The causes, by llm feature that needed a compiler change or a
workaround:

| Cause | Features |
|---|---|
| Compiler defects | 23 |
| Language a kernel could not use | 14 |
| Missing operations | 10 |
| Runtime | 9 |
| Codegen performance | 5 |
| Other | 3 |

Three findings shape this spec.

- **Defects are the largest cause, and many were silent.** 22 kernels
  vanished while the engine exited 0 (cajeta 4aba3c4e). A fused kernel read
  a neighbouring block's scale for 12 days (cajeta-llm 0f1ee39).
  `@Wave(width)` and `@Occupancy` were parsed and then ignored (a538aa77,
  a171d180). A library author cannot find these, let alone fix them.
- **Language gaps cost more than the commit count shows.** There were 20
  workarounds, each paid in silence: no ternary in a kernel, no buffer
  locals, helpers that cannot index a buffer, hand-unrolled loops, array
  literals refused as static initializers.
- **The operations added were for formats, not hardware.** `dotAccum`,
  `lut4`, `fromWords` and the scaled-accumulate epilogues served Q4_K,
  Q6_K and MXFP4 on hardware we already had. Under §1's rule those belong
  in a library.

### 1.2 Scope

- Kernel conformance: the compiler's own tests catch wrong lowering before
  a library author does (§2).
- Kernel language: every construct host code accepts either lowers in a
  kernel or is refused by name with a tracked item (§3).
- Building blocks: a set of general device operations that libraries
  compose, at no cost against a fused built-in (§4).
- An escape hatch: a library can reach a target intrinsic directly (§5).

### 1.3 Non-goals

- **Running a kernel on a device nobody has seen.** That is
  `xpu-kernel-adaptor`.
- **Choosing a tile's shape.** That is `xpu-tile-shape-selection`, which
  this spec feeds (§6).
- **New hardware.** A building block for a new hardware feature is a
  compiler change, and that is allowed.
- **Runtime APIs for engines** (streams, pools, buffers). They need the
  same testing discipline, but they are not kernel language.

### 1.4 The measure

The audit is rerun when the plan closes. Success is a new model or kernel
family landing in a library with no compiler commit, except building blocks
for hardware that is new. A compiler commit that a library change needed is
counted against this spec and classified by §1.1's causes.

## 2. Conformance: the compiler catches wrong lowering

Every kernel the compiler accepts must give the same answer on every
backend that lowers it, and a backend that cannot lower it must say so. Today
the kernels that exercise the most lowering live in cajeta-llm, and are
checked only in that repository's CI.

Use cases:
- 2.1 A corpus of real kernels runs in cajeta's own CI on every backend the
  runner has. It holds cajeta-llm's quant, attention and GEMM kernels and the
  stdlib's kernels, each with its host reference.
- 2.2 Each corpus kernel's output is compared against its host reference
  under a stated tolerance: bit-exact for integer work, a bound for float
  work. A disagreement fails the build and names the kernel and backend.
- 2.3 A silent outcome is a failure. A kernel that lowered on no backend, a
  declaration the backend never received (`@Occupancy`, `@Wave`), and a name
  in a kernel that resolved to something other than what the author named
  each fail by name. Each has a test that it fires and a test that it does
  not.
- 2.4 A kernel that lowers wrong on one backend can be held there by name,
  as cajeta-llm's `WRONG_HERE` does today. The hold lives in the corpus, so
  the compiler's own CI tracks it until it closes.

## 3. Language: what host code accepts, a kernel accepts

A kernel is cajeta, and an author should not have to learn which part.

Use cases:
- 3.1 A construct that host code accepts and kernel lowering refuses is a
  defect, with a tracked item. An inventory test lists every expression and
  statement kind and asserts that each lowers in a kernel, or appears in
  the refusal list with its item.
- 3.2 The known refusals close: the ternary operator, a `KernelBuffer`
  local, a static helper that indexes a buffer, an array literal as a
  static initializer, and the others in the inventory.
- 3.3 A tile is written once over its shape. A kernel in a class template
  reads its non-type parameters as constants, holds fragments in arrays
  that live in registers, and sizes arrays, shared tiles and annotation
  arguments with constant expressions over those parameters.
- 3.4 A library builds the instantiations it needs. Explicit instantiation
  of a class template lowers every listed instantiation, so a family of
  kernels can be built ahead of time without a compiler hook made for that
  family.

## 4. Building blocks: libraries compose, the compiler provides

A building block does one general thing that many kernels need. A finished
operation does one job for one format. Only the first kind is compiler work.

Use cases:
- 4.1 The building blocks a matrix-core kernel needs are present: fragment
  load, store and multiply; per-element fragment access with the lane-to-
  element map; integer multiply-add over packed bytes; byte permute; wave
  shuffle, reduce and ballot; conversions between every supported width;
  async copy and swizzled shared tiles.
- 4.2 A finished operation is rewritten as library code over building
  blocks. The first is `scaledAccumI32`, the Q4_K tile's integer fold.
- 4.3 Composition is free. The library version of a finished operation runs
  within a stated bound of the built-in on every backend where both exist,
  measured by interleaved wall time. Where it does not, the gap is a
  codegen defect, tracked under §2's rules, not a reason for a new built-in.
- 4.4 A new format, such as a new quantization scheme, lands as library
  code with no compiler commit.

## 5. Escape hatch: a library reaches the hardware

New hardware reaches libraries before it becomes a building block.

Use cases:
- 5.1 A kernel calls a target intrinsic by name, or embeds inline PTX or
  AMDGCN assembly (§7.5), with arms per backend and a portable arm. A backend with no arm and no portable arm refuses the kernel
  by name at build.
- 5.2 An intrinsic call is checked like any other operation: argument types,
  and the conformance rules of §2 where a reference exists.
- 5.3 When a building block covers the intrinsic, the library moves to the
  building block. The escape hatch stays for what nothing covers yet.

## 6. Relation to xpu-tile-shape-selection

`xpu-tile-shape-selection` began with the compiler work its tiles need.
That work is language work (§3.3, §3.4), so it moves here. The plan
records each moved unit:

| Was (tile-selection plan) | Now (this plan) | State |
|---|---|---|
| Unit 1, template values in kernels | Unit 3 (§3.3) | done, cajeta 75bd8c83 |
| Unit 2, fragment arrays | Unit 4 (§3.3) | done, cajeta 3f58538a |
| Unit 3, constant expressions | Unit 5 (§3.3) | open |
| Unit 4, the family and `@Shapes` | Unit 6 (§3.4), explicit instantiation | open; spelled `@Instantiate` (§7.3) |

Tile-selection keeps the feasibility filter, the measured choice and the
llm tile families. Its tile families wait on Units 5 and 6 here.

## 7. Decisions (Julian, 2026-10-03, in an interactive review)

- 7.1 The corpus home: cajeta's CI builds a PINNED cajeta-llm revision and
  runs every kernel it registers on each backend the runner has. The pin is
  bumped deliberately, and llm's build time joins cajeta's CI.
- 7.2 The reference semantics: the compiler grows a REFERENCE INTERPRETER
  that runs any kernel's source on the host, scalar, as the expected answer.
  A corpus entry is then a kernel and its inputs, with no hand-written
  reference. The interpreter shares the front end with the code under test,
  so a front-end defect can agree with itself; cajeta-llm's own host oracles
  stay in its CI as the independent check on that path.
- 7.3 Explicit instantiation is spelled `@Instantiate({Tile<4>, Tile<7>})`,
  usable on any class template, and a tile family is a class template that
  carries it; `ShapeChoice` reads the list. This replaces tile-selection's
  `@Shapes` (its 7.13).
- 7.4 Free composition means within 3% of the built-in, by interleaved wall
  time at the engine's shapes, the same line `ShapeChoice` uses for a tie.
- 7.5 The escape hatch reaches both target LLVM intrinsics and inline PTX
  and AMDGCN assembly. Intrinsic calls are typed and checked; inline
  assembly is checked only at its boundary (operand types and constraints),
  and a kernel using it names the backend arms it covers.
