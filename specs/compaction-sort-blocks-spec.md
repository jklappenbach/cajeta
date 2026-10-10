# Compaction, sort and type-coverage building blocks (spec)

Status: draft, written 2026-10-10 at Julian's request. He answered the open
questions the same day (9); the spec awaits his review.

## 1. Definition

### 1.1 Purpose

Every GPU program that filters, partitions or orders data on the device is
built from the same few cross-lane steps: each lane learns its rank among the
lanes that kept an item, one lane's value reaches every lane of the
workgroup, a scan runs across workgroups, and a radix sort is assembled from
those. cajeta has the wave and workgroup verbs underneath (`Wave.ballotSync`,
`Workgroup.reduce`, and `Workgroup.scan` and `Bits` over `uint64` from
int64-scan-verbs), but not these steps. Each kernel that needs one spells it
by hand, with a hard-coded mask width or a Shared slot and a barrier.

This spec adds them: a wave rank of a predicate, a workgroup broadcast, a
device-wide scan over a buffer, and a device-wide stable radix sort.

It also closes the type gaps in the verbs that exist, which were sized for
cajeta-llm's `float32` and `int32`: `Quad` over `float32` and `int64`, a
public `Wave.reduceMinF32`, `Workgroup.reduce` over `uint32` and `float64`,
`float64` atomics, and a packed `Vector` `toI16` (6).

### 1.2 Who needs it, read 2026-10-10

No kernel in the fleet uses these yet. The places that will:

- cajeta-llm's `gen/Sampler` sorts the whole vocabulary on the host with a
  heapsort for every sampled token, to apply top-k and top-p. On the device
  that is a sort of the logits row, or a partial selection.
- cajeta-xgboost's `sketch/Sketch` heapsorts each quantile queue on the host
  (`sortQueue`). Its GPU builder (XGBoost's own) sorts on the device.
- int64-scan-verbs Unit 7, cajeta-xgboost's row partitioner, scans per-block
  counts on the host between two launches. A device-wide scan removes the
  round trip.
- Any stream compaction: a filter that writes the survivors densely. It is
  the ballot of a predicate plus a count of the set bits below each lane, and
  a reservation in the output.
- The type gaps have no consumer yet. A float stencil moves floats across a
  quad through `Cajeta.f32ToBits` today, and double-precision science code
  has no `float64` reduce or atomic.

### 1.3 Constraints

- Every backend lowers the verbs: nvptx, amdgpu, vulkan and cpu, and the
  reference interpreter models them.
- Results are exact and identical on every backend at every wave width.
  Ranks, counts, integer scans and a stable sort have one right answer. The
  exceptions are a `float64` atomic sum, whose rounding depends on arrival
  order as a `float32` one does today, and a `float64` `Workgroup.reduce`,
  which fixes its combine order as the `float32` form does.
- A kernel names no wave width and no wave count to use them, and nothing
  assumes a wave fits in 32 bits of mask.
- The device-wide forms are library calls a host program makes, not verbs
  inside a kernel.

### 1.4 Non-goals

- Float scans and float reductions in a defined order (int64-scan-verbs 1.4).
- A comparison sort with a user comparator. Radix sort covers the key types
  in 5, and a comparator is a function value a kernel cannot take.
- A float sort order other than IEEE total order.
- Segmented forms: a scan or sort per segment of a buffer.
- `int64` atomics on Shared, which Vulkan gates on a device feature.

## 2. Wave rank of a predicate

### 2.1 Requirements

`Wave.rank(boolean p)` returns to each lane the number of active lanes below
it whose `p` is true. `Wave.count(boolean p)` returns to every lane the
number of active lanes whose `p` is true. Both are the ballot of `p` and a
bit count, below the lane's own bit for `rank`, at the wave's real width.
`Wave.laneMaskBelow()` returns the `uint64` with a bit set for each lane
below the calling one, for the ballot idioms the two verbs do not cover.

### 2.2 Use cases

- **2.2.1** When each lane passes whether it keeps an item, the keeping lanes
  receive 0, 1, 2 ... in lane order, so each writes its item at
  `base + rank` with no gap.
- **2.2.2** When no lane passes, `count` is 0 on every lane.
- **2.2.3** When the wave is 64 lanes wide, lanes 32 to 63 are ranked like the
  rest.
- **2.2.4** When a kernel computes `Bits.count(Wave.ballotSync(p) &
  Wave.laneMaskBelow())`, it equals `Wave.rank(p)` on every lane.

## 3. Workgroup broadcast

### 3.1 Requirements

`Workgroup.broadcast(T value, uint32 lane)` returns to every lane of the
workgroup the `value` that lane `lane` (in the workgroup's linear order)
passed. `T` is `int32`, `int64`, `uint32` or `float32`. It is a barrier like
`Workgroup.reduce`, and `lane` must be uniform across the workgroup.

### 3.2 Use cases

- **3.2.1** When lane 0 reserves output space with one atomic and broadcasts
  the base, every lane receives it and writes at `base + rank`.
- **3.2.2** When a kernel broadcasts twice in a row, or in a loop, the second
  call reads no value left by the first.
- **3.2.3** When `lane` is past the workgroup's last lane, the launch is
  refused by name, or the kernel is when `lane` is a constant.

## 4. Device-wide exclusive scan

### 4.1 Requirements

A host call, `DeviceScan` in `cajeta.xpu`, scans a `KernelBuffer<int32>` or
`KernelBuffer<int64>` of any length into an output buffer: element `i`
receives the sum of elements `0 .. i-1`, element 0 receives 0, and the call
returns the total. It runs on the device with no host round trip between its
launches, and its result is the same on every backend.

Two routes, chosen per backend (Julian's answer to 9.4):

- **Three launches**, on every backend: each workgroup reduces its tile, one
  workgroup scans the tile totals, and each workgroup scans its tile again
  and adds its tile's base.
- **Decoupled look-back**, on nvptx and amdgpu: one launch. Each workgroup
  publishes its tile's total, then its inclusive prefix, in a flag-and-value
  word per tile, and finds its base by reading its predecessors' words back
  to the first one that holds a prefix. A workgroup takes its tile index
  from an atomic ticket, not from its workgroup id, so it only ever waits on
  a tile that a workgroup already running has claimed. That is what keeps
  look-back from deadlocking when the device runs fewer workgroups at once
  than the launch has.

Both routes give the same result, since integer addition is exact.

### 4.2 Use cases

- **4.2.1** When a buffer of a million counts is scanned, each output equals
  the host's exclusive prefix sum, and the returned total equals the sum.
- **4.2.2** When the buffer is shorter than one workgroup, or empty, the
  result is still exact.
- **4.2.3** When cajeta-xgboost's partitioner scans its block counts with it,
  the partition is unchanged and the host never reads the block counts.
- **4.2.4** When the look-back route runs a launch of many more workgroups
  than the device holds at once, it completes, and its output equals the
  three-launch route's.
- **4.2.5** When the same buffer is scanned on every backend, every output
  element is the same.

## 5. Device-wide radix sort

### 5.1 Requirements

A host call, `DeviceSort` in `cajeta.xpu`, sorts a `KernelBuffer` of keys
in ascending order, stably, optionally carrying a `uint32` value per key
(typically the key's original index). Keys are `uint32`, `int32`,
`float32`, `uint64`, `int64` or `float64` (Julian's answer to 9.5). Signed
and float keys are mapped to an order-preserving unsigned form and back. A
float sorts in IEEE total order: -0 before +0, and NaNs after +infinity; a
negative NaN sorts before -infinity, as total order puts it. It is built from the
blocks in 2 to 4 and `Workgroup.scan`, so it runs on every backend, and its
output is identical on every backend because a stable sort has one answer.

### 5.2 Use cases

- **5.2.1** When a million `uint32` keys are sorted, the output is the host's
  stable sort of them.
- **5.2.2** When keys carry their indices, equal keys keep their input order.
- **5.2.3** When `float32` keys include -0, +0, infinities and NaNs, they sort
  in total order on every backend.
- **5.2.4** When `int64`, `uint64` and `float64` keys are sorted, including
  the extremes of each type and both zeros, each output equals the host's
  stable sort in that type's order.
- **5.2.5** When cajeta-llm sorts a logits row of 128k entries descending
  (by sorting negated keys, or reading the output backwards), the top-k
  indices equal the host Sampler's.

## 6. Type coverage

### 6.1 Requirements

Each existing verb gains the types below, with the semantics it already has
at its current types.

- **`Quad`**: `broadcast`, the three swaps over `float32` and `int64`. The
  float travels as its bits, the `int64` as two halves where a backend has
  no 64-bit form. Spelled with `F32` and `I64` suffixes, as `Wave` spells its
  typed forms, since a `uint32` method's `int32` and literal callers would
  move to an `int64` overload (measured for int64-scan-verbs 7.1).
- **`Wave.reduceMinF32`**: public, beside `reduceSumF32` and `reduceMaxF32`,
  on the same butterfly. The reference already evaluates it for
  `Group.reduce(GroupOp.Min, ...)`.
- **`Workgroup.reduce`**: over `uint32` (unsigned max and min) and `float64`
  (the `float32` form's fixed combine order). Measured 2026-10-10 with
  `float32`, `int32` and `int64` overloads: a `uint32` argument resolves to
  the `float32` form today, an `int32` or literal to the `int32` form. So a
  `uint32` overload moves today's `uint32` callers off a float reduce, and
  the plan finds those callers before it lands.
- **`float64` atomics**: `atomicAdd`, `atomicMin` and `atomicMax` on a
  `KernelBuffer<float64>`. Native where the device has them, a
  compare-exchange loop on the 64-bit word otherwise, and refused by name on
  a Vulkan device with neither the float64 atomic feature nor `int64`
  atomics.
- **`Vector.toI16`**: integer lanes truncated to their low 16 bits, as
  `toI8` truncates to 8.

### 6.2 Use cases

- **6.2.1** When a quad swaps `float32` values, each lane receives its
  partner's value bit for bit, NaN payloads included.
- **6.2.2** When a quad broadcasts an `int64` above 2^32, every lane of the
  quad receives both halves.
- **6.2.3** When a wave takes `reduceMinF32`, every lane receives the
  minimum, the same on every backend.
- **6.2.4** When a workgroup reduces `uint32` values above 2^31 with
  `GroupOp.Max`, the result is the unsigned maximum.
- **6.2.5** When a workgroup reduces `float64` values, the result is the same
  on every backend.
- **6.2.6** When many work-items `atomicAdd` small integers held as
  `float64`, the total is exact on every backend.
- **6.2.7** When `toI16` narrows `int32` lanes, each lane holds the low 16
  bits.

## 7. Refusals

- **7.1** `Workgroup.broadcast` inside control flow that not every lane of
  the workgroup reaches is refused, as a divergent barrier is.
- **7.2** A sort or scan of an unsupported element type is refused by name
  at compile time.
- **7.3** A `float64` atomic on a device that cannot perform one is refused
  by name at launch.

## 8. Adoption

None is required by this spec. 4.2.3 and 5.2.5 are measured against the
existing host code as conformance, and moving cajeta-llm or cajeta-xgboost
onto the device forms is a later decision per library.

## 9. Open questions

- **9.1** Scope. Should this spec also close the type gaps found on
  2026-10-10, or should they get their own spec? They are: `Quad` over
  `float32` and `int64`; a public `Wave.reduceMinF32`; `Workgroup.reduce`
  over `uint32` and `float64`; `float64` atomics; and a packed `toI16`.
  Recommended: their own spec. They are uniform type coverage, while this
  spec adds new operations.
  **Answered 2026-10-10 (Julian): fold them in here. Section 6.**

- **9.2** The wave rank's surface: `Wave.rank(p)` and `Wave.count(p)` as in 2,
  or the primitive `Wave.laneMaskBelow()` (a `uint64` of the lanes below this
  one) that kernels combine with `ballotSync` and `Bits.count`. Recommended:
  both. The verbs are the safe spelling, and the mask serves other ballot
  idioms.
  **Answered 2026-10-10 (Julian): both.**

- **9.3** Where the device-wide calls live: in the `cajeta.xpu` standard
  library as host classes that launch kernels (`DeviceScan`, `DeviceSort`),
  or in a separate library. Recommended: the standard library, beside
  `TileOps`, since the scan is what the partitioner needs.
  **Answered 2026-10-10 (Julian): the standard library.**

- **9.4** The device-wide scan's algorithm: reduce, then scan the block
  totals, then add (three launches, portable everywhere), or a single pass
  with decoupled look-back (faster on nvptx and amdgpu, but it relies on
  forward progress between workgroups, which Vulkan and cpu do not promise).
  Recommended: three launches, and look-back later as a per-backend route if
  a measurement asks for it.
  **Answered 2026-10-10 (Julian): look-back too, on nvptx and amdgpu, with
  three launches on Vulkan and cpu. Section 4.1.**

- **9.5** Sort key types. Recommended: `uint32`, `int32` and `float32` now;
  `int64` and `uint64` keys when a consumer needs them, since each doubles
  the passes.
  **Answered 2026-10-10 (Julian): `int64`, `uint64` and `float64` keys as
  well. Section 5.1.**
