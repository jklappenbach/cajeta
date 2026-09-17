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

```
Route {
  name       — what a refusal or a route record prints
  regime     — decode-row (M = 1) | prefill-batch (M ≥ tile) | bind | fused tail
  admits     — (format, shape) → admitted | refused-by-clause
  needs      — the Capability set the device must satisfy
  dispatch   — the launcher; every admitted format has an arm
  priority   — order among rows admitting the same (format, regime)
}
```

`RouteTable.pick(regime, format, shape)` returns the first admitting row
by priority, or a refusal naming the last row consulted and the clause
that refused. Route records read the same object: a census that shows
the wrong kernel is explained in one line, not by bisection.

## 3. The audit

For every format the package admits and every row: admitted or refused
by name; and for every admitted pair, the dispatcher has an arm. A bare
`else` in a dispatcher fails the audit on its first run. Adding a format
means the audit names every row that lacks it; adding a row means the
audit runs it against every format that exists. Per row, a test that
the route fires and a test that it does not.

## 4. What it is not

It does not write kernels, choose data layouts, or select among admitted
rows by cost — the last is the tile family's, and it needs the rows to
exist first. It does not absorb capability queries: a row's `needs`
names a `Capability`; the device answers it.

## 5. Beyond one package

Only the format axis is language-model-specific. Histogram kernels
chosen by bin count and feature count, dense GEMMs chosen by dtype and
WMMA availability, attention chosen by head dimension — each is this
table written as if-chains. The row type is generic from the start; a
package registers rows, and the audit is the same test.
