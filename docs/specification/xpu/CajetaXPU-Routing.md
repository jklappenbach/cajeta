# cajeta.xpu — Kernel Routing

How the layer decides which kernel runs. Companion to
[CajetaXPU.md](CajetaXPU.md) (the substrate) and the
[kernel-writing guide](../../guide/25-xpu-kernels.md) (how to write one).
Specified by [`specs/route-table-spec.md`](../../../specs/route-table-spec.md).

## 1. Three questions, three homes

Every package with more than one kernel for one operation answers the
same three questions, and the layer keeps them apart because they have
different owners and different lifetimes.

| question | owner | surface |
|---|---|---|
| **Capability** — can this device run kernel K? | the device | `Device.supports(Capability)`; the geometry surface (`waveSize`, `simdCount`, `dispatchBlocks`, `sharedBytesPerBlock`, `l2CacheBytes`, `integrated`) |
| **Cost** — how fast is K at shape S here? | measurement | the tile manifest (VGPR / spill / LDS from the code object), the scheduler's measured ridge and calibration set, the `Autotune` store |
| **Policy** — which admitted kernel runs? | the package | `Route` / `RouteTable` |

A backend name answers none of them. A predicate that tests
`activeBackend()` is asking a capability question by proxy and will be
wrong on the next device that has the capability under another name.

## 2. A route is a row

One kernel **variant**, serving one format, in one regime. Q4_K's wave
variant and its packed variant are two rows; `priority` orders them.

One variant per row is what lets `dispatch` have no arms, and therefore
no bare `else` for a new format to fall through — which is the shape of
defect the table exists to stop. It is also what a weight stores in
place of the sixteen booleans it resolves at bind today.

| field | meaning |
|---|---|
| `name()` | what a refusal, a route record or the audit prints |
| `regime()` | `DecodeRow` (M = 1) \| `PrefillBatch` (M ≥ tile) \| `Bind` \| `FusedTail` |
| `format()` | the ONE format this variant serves — an opaque `int32` to the layer |
| `shapeRefusal(query)` | the static half: null when the shape fits, else the gate that refused |
| `needs()` | the `Capability` list the device must satisfy; a stored list, so asking costs no allocation |
| `readyRefusal(call)` | the runtime half: null when ready, else the gate that refused |
| `dispatch(call)` | the launcher. One variant, one kernel: nothing to choose here |
| `priority()` | order among rows serving the same (format, regime) |

A route is an **interface a registrant implements**, never a record of
function values: a dispatcher launches kernels, and a kernel launch
inside a lambda has unreachable captured buffers in launch codegen.

### 2.1 Admission has two halves, and they are different questions

`shapeRefusal` is pure over declared facts and is what the audit walks
with nothing bound. `readyRefusal` reads mutable state — the receiver's
slab, a widen that was declined, a global A/B switch someone can flip at
run time — and is a runtime refusal no audit can check. Tangling them is
the defect, not an incidental of it: a predicate that names formats
*and* reads a slab cannot be widened for a new format without also
claiming its slab is ready, which is how one format's bytes reach
another's decoder.

The test for which half a constraint belongs in is not "is this value
known at compile time". It is **can the audit's answer change depending
on when it ran**. A field on the query cannot; a mutable static can, and
so belongs in the runtime half however constant it looks.

The split is enforced by TYPE as far as a type can enforce it.
`shapeRefusal` takes a `RouteQuery` — regime, format, and whatever
static facts the registrant extends it with. `readyRefusal` and
`dispatch` take a `RouteCall`, which carries a query plus the receiver
and the operands. So a static half cannot reach the RECEIVER, by
construction; reaching a global is still the author's to avoid, and the
test above is how.

A `RouteCall` **holds** a query rather than extending one: a
registrant's call type has to extend `RouteCall` and still carry its own
query subclass, which single inheritance forbids the other way round.

### 2.2 A half answers with the gate, not with a boolean

Both halves return a `String`: null when they admit, otherwise the row's
own name for the constraint that refused. A row usually has several
gates in one half — the zero-sync MoE row tests gating, hidden
alignment and expert width statically, and a slab bind and a widen at
runtime — and a boolean answer would coarsen them into one word, which
is the complaint this table exists to answer. Naming costs nothing: a
cajeta string literal materializes as a static view-mode `String`, not
an allocation, and there is exactly one statement of each gate rather
than a predicate and a matching list of reasons that can drift apart.

## 3. Resolution, and the refusal

`RouteTable.admissible(query)` is the static half — format, regime,
`shapeRefusal`, `needs` — and answers with nothing bound, which is what
the audit walks. `RouteTable.pick(call)` is that plus `readyRefusal`,
and is what a registrant calls **at bind** to resolve a weight's row
once and store it; the launch is then one virtual call, not a table
walk. Both read one implementation of the static test, `clauseOf`, so
they cannot drift. `candidates(query, out)` returns the whole admissible
set, highest priority first — `priority` is a declared default, and on
hardware nobody has measured it is a guess, so measured selection is an
addition rather than a rewrite.

Either returns the row or nothing. `whyNotAdmissible(query)` and
`whyNotPicked(call)` build the `RouteRefusal`, on the path that already
refused and is about to print something; resolution itself allocates
nothing. A refusal carries the row, the clause — `Format`, `Regime`,
`Shape`, `Unsupported`, `Ready`, or `Empty` for a table with no rows —
the regime and format asked for, the row's own gate text, and the index
into `needs()` of the capability that was missing.

The row a refusal names is the one that got **furthest** through the
clauses, never the one consulted last. Registration order does not reach
the selector's answer, and a diagnostic that reintroduced it would put
that order back in the one place a reader trusts.

Route records read the same object: a census that shows the wrong kernel
is explained in one line, not by bisection.

## 4. The audit

`RouteTable.audit(queries)` walks the **declared half** — format,
regime, shape — over the registrant's own queries, and returns a report:
per (query, row) the clause that answered; per query how many rows serve
it, and whether the choice was shadowed; per row how many queries it
fires for, and what capabilities it declares it needs.

Declared-only is what makes it a host test, and the line is sharper than
"no bound weight". `needs` is also excluded, because it asks the live
device: a process that selected no backend answers false to every
capability, and one on the wrong part answers differently again. An
audit that ruled on `needs` would call a whole table unserved on a
machine without the hardware — the machine you are most likely to be
adding a format on. A row whose capability the local box lacks still
serves its format; what is missing is silicon, not a row.

So the audit **reports** each row's declared `Capability` set rather than
ruling on it, and `needs` stays in `admissible` and `pick`, where asking
the live device is the whole point. The result is that adding a format
names every row that lacks it before any model is loaded, from any
machine; adding a row runs it against every format that exists.

There is no dispatcher probe. At one row per kernel variant, a row that
admits a format has exactly one kernel for it and no arm to omit.

`auditRow(row, queries)` is the per-row **fire / no-fire** pair: hand it
the row's own format at shapes that should and should not take it. A row
that answers zero, or answers every one, has a gate that is not
discriminating — a predicate that silently disabled a whole check once
read as a clean run for an hour.

## 5. What it is not

It does not write kernels, choose data layouts, or select among admitted
rows by cost — the last is the tile family's, and it needs the rows to
exist first. It does not absorb capability queries: a row's `needs`
names a `Capability`; the device answers it.

## 6. Beyond one package

Only the format axis is language-model-specific. Histogram kernels
chosen by bin count and feature count, dense GEMMs chosen by dtype with
forward and backward regimes, attention chosen by head dimension — each
is this table written as if-chains. The layer knows a regime and an
opaque `int32`, so the row type is generic from the start; a package
registers rows, and the audit is the same walk.
