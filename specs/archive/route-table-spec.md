# Route table — spec

Status: approved 2026-09-18, **§3 amended 2026-09-18** (Julian) — see
§3.0, whose third correction was found building Unit 2 and whose
fourth was found by a question about `Device.supports`. Closed 2026-09-18; the plan is archived beside it.
Plan: [`agents/archive/route-table-plan.md`](../../agents/archive/route-table-plan.md).
Layer: `cajeta.xpu`. First registrant: cajeta-llm. Architecture note:
[`docs/specification/xpu/CajetaXPU-Routing.md`](../../docs/specification/xpu/CajetaXPU-Routing.md).
Companion guide: [Writing XPU kernels](../../docs/guide/25-xpu-kernels.md).

## 1. Definition

The route table is the xpu layer's selection contract: one declaration
of which kernel serves which data format in which regime, and one test
that audits that declaration against every format a package admits. It
replaces the predicates that today restate that knowledge from memory,
one per route, in whichever file the route lives.

It is the third of three questions the layer separates. **Capability**
— can this device run kernel K — is `Device.supports(Capability)` and
the geometry surface. **Cost** — how fast is K at shape S here — is the
tile manifest (footprint from the code object), the scheduler's measured
ridge and the `Autotune` store. **Policy** — which admitted kernel runs
— is this table. The first two exist; the third is if-chains in every
package that has two kernels for one operation.

### 1.1 Problem statement

A format reaches parity in four parts — decoder, host mat-vec, bind
gate, device route — and the fourth part is not one thing. It is a
dozen: the decode wave route, its integer and f32 arms, the grouped id
route, the fused row route, the coop GEMM, the WMMA widen, the batch
kernels, the split at bind. Each has a predicate that admits a format,
and each predicate was written when the route was, naming the formats
that existed then.

The consequence is measured, not argued. On 2026-09-17 alone:

- `wavef`'s guard read `!q8kDims` where it meant "no integer wave route
  engaged"; IQ4_NL answered no to both and decoded item-per-row at a
  256-aligned width. 26.4 → 42.9 t/s once asked properly (plan 9.2.4).
- `ExpertBank.idReady()` listed types 12 and 14. The codebook banks were
  refused the grouped id route and dispatched one launch per expert per
  projection: 384 a token against llama.cpp's ~72 (plan 6.4.3, cause 3).
- `Linear.packedWaveReady()` ended `(q8 && wave) || wave6` — the same
  list, one file over — and its dispatcher fell through a bare `else`
  into the Q6_K decoder. Widening the predicate alone would have fed
  codebook bytes through it: wrong logits, no crash.
- The fix for the second added `codebookId()`: one more list of the
  same shape.
- `ExpertBank.idGemmReady()` reads `(packedTy == 12 || packedTy == 14)`;
  the grouped prefill id-GEMM refuses the codebook banks, which fall
  to one coop launch per expert — sixteen workgroups each, 8.9 GB/s
  against a 206 GB/s ceiling, 3810 of them in series. Cause 5 of the
  same item, found by arithmetic the same afternoon.

This is the eighth or ninth time the class has been solved, and each
time from scratch, because the knowledge lives in prose and in whoever
last hit the refusal. The half that exists is `theFourPartInvariant`
(codebook-quants 3.1.5): over every `supported()` type, four gates
agree. It has held since Unit 3. It covers four gates; the routes have
grown to a dozen predicates outside it, so it stays green while a
route it does not know refuses.

### 1.2 Scope

`Route` and `RouteTable` in `cajeta.xpu`: the row type, the selector,
the refusal, and the audit's walker. Packages register rows and own
the dispatchers the rows name. cajeta-llm is the first registrant, and
every predicate there that decides which kernel runs for a weight is in
scope — the measured history in §1.1 is why it goes first.

### 1.3 Non-goals

- **Capability queries.** `Linear.backendIsVulkan()` at ~14 sites asks
  "is WMMA present", "can I `dotSum`", "is this RADV". Those are
  `Device.supports(Capability)` questions, and the `Capability` enum
  grows the values they need (`CoopMatrixBf16F32Acc` and
  `AtomicInt64` are the shape). A row's `needs` field names them; the
  sites themselves are a separate change, deferred to §7.
- **Measured selection.** Choosing among admitted routes by the
  per-device Autotune store. The table makes that possible; it does
  not do it.
- **Layout selection.** The resident split layout is already one
  layout for every format (codebook-quants Unit 9). A layout exists
  because kernels read it; choosing one is choosing a kernel set, and
  the kernels still have to be written.
- **Writing kernels.** The table names which kernel serves a format.
  It does not draft the kernel's body.

## 2. Possible solutions

### 2.1 A sweep onto `qAct` / `intWaveRouted()`

Replace each format list with the union predicate `Linear` already
carries. Fixes every list that exists today. Does nothing for the next
route, whose author writes a new predicate outside the union — which
is how `idReady` came to exist beside `qAct`. Rejected as sufficient;
kept as the first mechanical step of §4.

### 2.2 Extend the four-part invariant to every predicate

Keep the predicates, teach the test about all of them. The test then
has to *know* every predicate, and a new one is invisible to it until
someone adds it — the same failure, moved into the test. Rejected.

### 2.3 A route table (proposed)

Routes become rows of data. There is one selector and one audit, and
the audit walks the table, so a route that exists is a route the test
sees. §3.

### 2.4 Measured selection over the table

Choosing among admitted rows by the tile manifest's footprint, the
scheduler's ridge and the `Autotune` store. Correct, larger, and it
sits *on top of* §2.3 — the rows are the policy half, and without them
there is nothing to select among. Deferred to §7.

## 3. Proposed solution

### 3.0 Amendment: granularity, and the two halves of admission

The first implementation of §3.1 was measured against the call sites it
has to replace and did not fit them. Two corrections, both load-bearing.

**A row is one kernel variant admitting ONE format**, not a family
admitting a set. `Linear.launchOne` dispatches on sixteen booleans —
`q8`, `packed1`, `wave`, `wave6`, `wave2`, `wave3`, `wave5`, `wave8`,
`waveT1`, `waveT2`, `wave40`, `wave50`, `wave4nl`, `wavef`, `waveH`,
`waveIq` — computed once in `ensureDevice` and read at every launch.
Those booleans ARE the route, and they are what a row replaces: the
weight resolves its row at bind and stores it, and the launch is one
virtual call. A family-with-arms row leaves the arms, which means it
leaves a bare `else` to fall through and needs every arm audited by a
dry-run flag it can forget to honour. At one row per variant there are
no arms: Q4_K's wave row and its packed row are two rows of the same
format, `priority` orders them, and the shadowed-row report of §3.4.6
is exactly that choice.

**Admission has a static half and a runtime half, and they are
different questions.** `ExpertBank.idReady()` today answers both at
once: it names formats AND reads `slabDev`, `deqSlabDev`,
`widenRefused` and the static `widenSlabOn`. `zeroSyncReady` goes
further and calls `ensureSlab()`, which binds and can fail. Tangling
them is the defect, not an incidental of it — it is why widening the
predicate alone would have fed codebook bytes through the Q6_K decoder.
So a row answers them separately: `fits` is pure over declared facts
and is what the audit walks with nothing bound; `ready` reads bound
state and is a runtime refusal the audit cannot and must not check.

The split is enforced by TYPE, not by discipline. `fits` takes a
`RouteQuery` — regime, format, and whatever static facts the registrant
extends it with. `ready` and `dispatch` take a `RouteCall`, which
carries the query, the receiver and the operands. A row's `fits` cannot
reach bound state because its parameter has no receiver to reach it
through.

**The audit rules on what is DECLARED, never on the device** — found
when Julian asked whether `Device.supports` returning false for
everything was a bug. It is not: a program with no kernels bundles no
backend, so the device is `none` and every capability answers false,
while the same query on the CPU backend answers true for two of four.
But it exposed that `needs` sat in the half the audit walks, so the
audit's verdict moved with the box it ran on — and it was documented as
device-free. `needs` stays in `admissible` and `pick`; the audit walks
format, regime and shape, and reports what each row declares it needs.
See §3.3.

**A half answers with the GATE that refused, not with a boolean** — and
this one was found building the refusal (§3.4.4), against
`zeroSyncReady`, which is the predicate the row has to replace. That
predicate has nine gates, each with its own sentence in the
`moe-row-route` record: four are shape tests, three are per-bank format
tests, two read a slab. Four collapse into `fits` and two into `ready`,
so a half that answered `false` would coarsen six distinct sentences
into two — the opposite of §1.1's complaint, delivered by the mechanism
that was supposed to fix it. So both halves return a `String`: null
when they admit, otherwise the row's own name for the gate. Naming
costs nothing — a cajeta string literal is a static view-mode instance,
not an allocation — and there is one statement of each gate rather than
a predicate and a matching list of reasons that can drift apart.

### 3.1 A route is a row

| field | meaning |
|---|---|
| `name` | what the diag prints when it refuses or takes it |
| `regime` | decode-row (M = 1), prefill-batch (M ≥ tile), bind, fused tail |
| `format` | the ONE format this variant serves |
| `shapeRefusal(query)` | the static half: null when the shape fits, else the gate that refused (`inDim % 256`, `outDim % tile`, block alignment, a fused row's co-formats) — pure, and auditable with nothing bound |
| `needs` | capabilities the device must have — queries, never a backend name; a stored list, so asking costs no allocation |
| `readyRefusal(call)` | the runtime half: null when ready, else the gate — this weight's slab did not bind, its widen was refused, the static switch is off |
| `dispatch(call)` | the launcher. One variant, one kernel: no arms, so no bare `else` |
| `priority` | order among rows serving the same (format, regime) |

A fused row keys on the format of the tensor that names it and puts the
co-formats of its other operands in `fits`, where the audit can read
them — which is how `idDownCombineTail` should have been declared.

### 3.2 One selector

Two entry points over one test, so they cannot drift.
`RouteTable.admissible(query)` is the static half — format, regime,
`fits`, `needs` — and answers with nothing bound, which is what the
audit walks. `RouteTable.pick(call)` is that plus `ready`, and is what
a registrant calls AT BIND to resolve a weight's row once and store it.
Either returns the highest-priority row, or nothing; the refusal is
built by `whyNotAdmissible` / `whyNotPicked` on the path that already
refused, so resolution itself allocates nothing. A refusal names the
row that got FURTHEST through the clauses — format, regime, shape,
capability, readiness — and never the row consulted last, because
registration order does not reach the selector's answer and must not
reach the diagnostic's either. The `moe-row-route` and
`moe-batch-route` records read the same object. No caller tests a
format itself.

### 3.3 One audit

`theRouteTableInvariant`: for every `ty` in `Quant.supported()` and
every row, the row either admits `ty` or refuses it by name.

The audit walks the **declared** half — format, regime, shape — over
registrant-built queries. Not the runtime half, which depends on a
bound weight; and **not `needs`**, which depends on the device the
process happened to select. Both would make the audit answer differently
depending on where it ran, and neither is a property of the table. A row
whose `needs` the local box cannot satisfy still counts as serving its
format, because it does: what it lacks is hardware, not a row. Ruling on
that would report a whole table unserved on the machine you are most
likely to be adding a format on.

What a row needs is **reported, not ruled on** — the audit lists each
row's declared `Capability` set, so "this format has a row, and that row
wants `AtomicInt64`" is answerable without the part in hand. `needs`
stays in `admissible` and `pick`, where asking the live device is the
whole point.

There is no dispatcher probe: at one row per kernel variant, a row that
admits a format has exactly one kernel for it and no arm to omit. Per
row, a does-fire test and a does-not-fire test, as codebook-quants
9.2.7 did.

### 3.4 Use cases

- **3.4.1** When a format is added, the audit names every row that
  lacks it, before any model is loaded.
- **3.4.2** When a route is added, the audit runs it against every
  format that exists, in the same commit.
- **3.4.3** When a dispatcher has no arm for a format its row admits,
  the audit fails; the fallthrough of 9.2.4 and `packedWaveReady`
  cannot ship.
- **3.4.4** When a route refuses at runtime, the diagnostic names the
  row and the clause, so a census that shows the old kernels is read
  in one step, not by bisection.
- **3.4.5** When a predicate would name formats, it is a row instead;
  a grep for `== 12 || == 14`, `codebookId`, `(q8 && wave) || wave6`
  returns nothing.
- **3.4.6** When the same (format, regime) is admitted by two rows,
  priority decides and the audit reports the shadowed row — the seed
  of measured selection (§7), without doing it yet.

## 4. Implementation

### 4.1 Where

`cajeta.xpu.Route` (the row) and `cajeta.xpu.RouteTable` (rows,
selector, refusal, audit walker) in the runtime, beside `Autotune` and
`Capability`. The llm package registers its rows in
`dev.cajeta.llm.model` and keeps its dispatchers. The mechanism ships
generic on day one; a second registrant (§6) costs no move.

### 4.2 Order

1. §2.1's sweep, so the table starts from predicates that mean what
   they say.
2. Decode-row routes: `launchOne`'s chain, `packedWaveReady` /
   `matvecPackedKeep`, `idReady` / `idRowReady` / `symId`, the
   `zeroSyncReady` clauses. This is where every defect of 2026-09-17
   lived.
3. Prefill routes: `coopRoutedHere`, `hasBatchKernel`, the widen and
   Mw8 gates, the coop tile-divisibility refusals.
4. Bind: `splitOn` (which 9.2.1 already reduces to "nonzero
   `scaleBytes`") and the `deqFor` twin.
5. Attention: the flash decode / prefill tile gates on `hd == 128` —
   the same shape of predicate, recorded under codebook-quants 4.3.5
   as the reason bitnet-large (head dim 96) takes the scalar pair.

Each step replaces predicates with rows and every bare `else` with
arms and a terminal refusal; the audit is extended as rows land, so
the invariant never covers less than it did the commit before.

### 4.3 Tests

`theFourPartInvariant` becomes `theRouteTableInvariant` and grows with
§4.2. Per row: fire / no-fire. The existing route tests
(`MoeRowRouteTest`, `LinearKernelRouteTest`) keep asserting decisions;
they read the table instead of the flags.

### 4.4 Acceptance

- The audit is green over every `supported()` type and every row.
- The grep of 3.4.5 returns nothing outside `RouteTable`.
- Filtered suite green.
- Legs flat on the recorded files (the six of codebook-quants 9.3.2
  and the Qwen1.5-MoE): this is a refactor, and a speed change in
  either direction is a routing change to explain.

## 5. Kernel classes it helps draft

"Helps draft" means: when the kernel exists, the table says where it
goes, what it admits, and what it must not silently replace — and the
audit says what is missing. The classes, by regime:

- **Decode mat-vec** — the wave kernels (integer and f32 arms), the
  grouped id kernels, the fused down + combine + norm tails. The
  densest set of predicates today and the whole of 6.4.3's cause 3.
- **Prefill GEMM** — coop X1 / X3 / N64 / N256, the WMMA Mw / Mw8
  widen routes, MMQ. Their tile-divisibility refusals become row
  constraints instead of scattered `% 128` checks.
- **Bind** — the resident split, the widen slab, the retired repack.
- **Attention** — flash decode / prefill tile against the scalar pair,
  gated on head dimension. Not a weight format, but the same decision
  and the same failure mode (bitnet-large silently on the slow path).
- **Fused tails** — a row whose `admits` is the conjunction of the
  rows it fuses, which is how `idDownCombineTail` should have been
  declared from the start.

What it does not help: the kernel body. An IQ3_XXS id kernel still has
to be written by someone who knows the block layout.

## 6. Beyond language models

The table's shape is (data format × regime × capability) → kernel, with
an audit over the format set. Only the format axis is LLM-specific —
GGUF quantizations. The mechanism is not:

- **cajeta-xgboost** picks histogram kernels by bin count, feature
  count and device (the int64 fixed-point quantiser is one such route),
  with the same "which one runs here" question and no audit.
- **cajeta-ml** picks dense GEMMs by dtype (f32 / f16 / bf16), shape and
  WMMA availability. Training adds a regime axis (forward / backward)
  that maps directly onto §3.1's `regime`.
- Any package that has more than one kernel for one operation has this
  table already, written as if-chains.

So: the mechanism is xpu's from the start, and the llm package is the
first registrant because that is where the pain has been measured
eight times. xgboost is the second candidate; its rows are the proof
that the row type is not shaped like a GGUF quantization.

## 7. What follows

- The `backendIsVulkan` sites become `Device.supports` queries and the
  `Capability` values they need; driver quirks move with them.
- Measured selection among admitted rows (§2.4), fed by the tile
  manifest, the scheduling calibration set and `Autotune`.
- The `xpu-tile-workload-profiles` multimodal-ML witness reads its
  routes from this table rather than from the llm package's flags.
- **Fleet calibration, and it is the gap for datacenter scale-out.**
  `candidates` plus `Autotune` lets one machine discover the best
  admissible row for its own part and recall it. It does not let a
  fleet SHARE that: every node rediscovers it, per process, and there
  is no versioning tying a recorded winner to the kernel set that was
  measured. A new part arriving across a fleet therefore pays its sweep
  once per node rather than once. Not this spec's, but it is the first
  thing that hurts at scale and nothing else currently owns it.

Each is its own spec; each assumes this table exists.
