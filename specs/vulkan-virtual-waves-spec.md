# Vulkan virtual waves (spec)

Status: approved by Julian 2026-10-08.

## 1. Definition

### 1.1 Purpose

A kernel that declares `@Wave(width = W)` is written against a wave of exactly
W lanes. Its shuffles, ballots and reduces mean W lanes, and its float reduce
has one defined order over W lanes (the xor butterfly of
ReferenceInterpreter.md). On Vulkan the compiler asks the driver for subgroups
of W through `requiredSubgroupSize`. A device that cannot honour the request
refuses the launch by name (v0.37.0, `Device.checkLaunch` reason 3).

That refusal is honest, and it is a gap. lavapipe runs every subgroup at 8
lanes. A device without `subgroupSizeControl`, or whose range excludes W,
runs at a width the kernel did not ask for. Those devices cannot run any
kernel that pins a wave.

A virtual wave closes the gap. When the device cannot run W natively, the
kernel still runs as written, on a wave of W logical lanes built from the
device's real subgroup, and its results match the reference bit for bit.

### 1.2 Terms

- **W**: the declared wave width.
- **S**: the subgroup width the device actually runs the pipeline at.
- **Logical lane**: one of the W lanes the kernel sees. `Wave.laneId()`
  returns a logical lane.
- **Wide case**: W > S. Each invocation carries C = W / S logical lanes.
- **Narrow case**: W < S. One subgroup holds S / W logical waves.

### 1.3 Who needs it, measured 2026-10-08

- lavapipe (llvmpipe, 256 bits) runs subgroups at 8 lanes and nothing else.
  Every `@Wave(width = 32)` kernel is refused there today.
- RADV on gfx1151 can pin 32 and 64. It needs no virtual wave.
- A device whose subgroup width is fixed, or whose size control range
  excludes W, refuses every kernel pinned to another width.
- On cajeta-llm main (7b474b5), 32 of 257 kernels declare
  `@Wave(width = 32)`. llm's own audit (its wave-row-geometry 2.2.5) found that
  every other kernel using a wave op derives its lanes from the device.
  ml, xgboost and codec declare none. An earlier correction here said no
  llm kernel declares one. That was measured on a feature branch 81 commits
  behind main and was wrong.

### 1.4 Constraints

- The kernel source does not change. A virtual wave is the backend's
  business.
- Results match the reference interpreter at W bit for bit, for every
  wave verb and for `Workgroup.reduce`.
- A device that can run W natively keeps the native path. A virtual wave is
  never chosen when the device can pin W.
- One compiled artifact serves every Vulkan device. S is learned at launch.

### 1.5 Non-goals

- Speed. A virtual wave is a portability path. It must be correct and it
  should not be slower than it has to be, and no performance target is set.
- Virtual waves on cpu, amdgpu or nvptx. cpu already runs any W as a vector
  of W lanes. amdgpu and nvptx run their native widths.
- Changing the reduce order of the native Vulkan path. See §6.2.

## 2. Selection

### 2.1 Requirements

- 2.1.1 At pipeline creation the runtime reads the device's subgroup range
  and size control. When it can pin W, the native pipeline runs.
- 2.1.2 Otherwise the runtime selects the virtual variant for the device's S.
- 2.1.3 When no variant fits, the launch is refused by name as v0.37.0 does.
- 2.1.4 The launch record names the path taken, native or virtual at S, so
  a test can assert it. Nothing is written to stderr.

### 2.2 Use cases

- 2.2.1 When a `@Wave(width = 32)` kernel launches on RADV, it runs natively
  at 32.
- 2.2.2 When the same kernel launches on lavapipe, it runs as a virtual wave
  of 32 over subgroups of 8, and its output matches the reference.
- 2.2.3 When a kernel declares no wave width, nothing changes.
- 2.2.4 When a test asks which path ran, the launch record names it.

## 3. The wide case (W > S)

### 3.1 Requirements

- 3.1.1 Each invocation carries C = W / S logical lanes. Logical lane `l`
  lives in invocation `l % S`, slot `l / S`. One logical wave is one real
  subgroup, so no wave verb needs a workgroup barrier.
- 3.1.2 A workgroup of B logical lanes dispatches B / C invocations.
  `Workgroup.dimX` and the thread and global ids report logical values.
- 3.1.3 Logical lanes inside one invocation may diverge. Each runs its own
  path, and a wave verb sees exactly the logical lanes that reached it.
- 3.1.4 Every wave verb keeps its reference meaning over W logical lanes:
  shuffles (index, xor, up, down, and the F32 forms), `rotate`,
  `ballotSync`, every integer reduce, the float reduces and their segmented
  forms, `prefixSum` and `prefixProduct`, the Quad verbs, the `Group` verbs,
  `Barrier.wave` and `WaveVector`.
- 3.1.5 A float reduce runs the reference butterfly. With the layout of
  3.1.1, partners at distance 1 to S/2 are other invocations, reached by
  subgroup shuffle. Partners at distance S to W/2 are slots in the same
  invocation, combined in registers. The stages run in reference order.
- 3.1.6 `Workgroup.reduce` sizes its scratch by logical waves and combines
  them as on every other backend.
- 3.1.7 A kernel the virtual lowering cannot carry is refused at compile
  time by name, with the construct named, and keeps its native variant.

### 3.2 Use cases

- 3.2.1 When a kernel reduces a float across its wave on lavapipe, the
  result matches the reference bit for bit.
- 3.2.2 When half the logical lanes of an invocation take a branch and
  shuffle inside it, each reads the lane the reference reads.
- 3.2.3 When a kernel ballots, the mask has one bit per logical lane in
  logical lane order.
- 3.2.4 When `Workgroup.reduce` runs at block 256 and W 32 over S 8, it
  combines 8 logical waves and matches the reference.
- 3.2.5 When the whole reference corpus runs on lavapipe, every kernel that
  pins a width passes or is refused by name.

## 4. The narrow case (W < S)

### 4.1 Requirements

- 4.1.1 Logical wave `k` of a subgroup is the aligned span of W real lanes
  starting at `k * W`. `Wave.laneId()` is the subgroup lane modulo W.
- 4.1.2 Every verb stays inside its span. A shuffle up or down past the
  span's edge reads the lane itself, as at the wave's edge today. A ballot
  returns the span's W bits.
- 4.1.3 A float reduce is the butterfly stopped at W. This is the segmented
  reduce the language already has, with `seg = W`.
- 4.1.4 An integer reduce combines within the span, through clustered
  subgroup operations or the butterfly.

### 4.2 Use cases

- 4.2.1 When a `@Wave(width = 32)` kernel runs on a device fixed at 64, two
  logical waves share each subgroup and neither sees the other's values.
- 4.2.2 When it reduces a float, the result matches the reference.

## 5. Proof

### 5.1 Requirements

- 5.1.1 The reference corpus runs on lavapipe through the virtual path and
  compares to the reference at 0 ulp.
- 5.1.2 The narrow case is forced on RADV, which can run 64, so a 32-lane
  kernel is tested at S = 64 on real hardware.
- 5.1.3 The wide case is forced on RADV at S = 32 for a 64-lane kernel, so it
  runs on real hardware too.
- 5.1.4 Each refusal has a test that it fires and a test that its twin does
  not.

### 5.2 Use cases

- 5.2.1 When the forcing switch selects the virtual path on a device that
  could run W natively, the output still matches the reference.
- 5.2.2 When cajeta-llm's test suite runs on lavapipe, its kernels run
  instead of being refused.

## 6. Findings that bear on this work

### 6.1 The native Vulkan float reduce has the driver's order

`SpirvKernelLowering.cpp:1052` lowers `Wave.reduceSumF32` and its max and
min forms to `OpGroupNonUniformFAdd` (and `FMax`, `FMin`) Reduce. The order
of that reduce is the driver's. amdgpu does the same with
`amdgcn_wave_reduce_fadd`. ReferenceInterpreter.md already says so: those
two backends "carry a stated bound". `Workgroup.reduce` calls the same hook
(`KernelLowering.cpp:1131`).

The v0.37.0 corpus measured cpu, amdgpu, nvptx and RADV bit-identical at
W = 32. That is a measurement on two drivers, not a property of the
lowering. workgroup-reduce decision 6.5 and the v0.37.0 notes state it as a
property. A virtual wave cannot borrow the driver's order, because the
driver reduces S lanes, not W. So it runs the butterfly by construction.

### 6.2 Out of scope here

Whether the native amdgpu and Vulkan float reduces move to the butterfly is
a separate decision, recorded as §7.5.

## 7. Decisions (Julian, 2026-10-08)

- 7.1 **The wide-case mechanism.** Each invocation carries its C logical
  lanes as a vector of C, as cpu carries a wave as a vector of W. Divergent
  logical lanes are masked, and the wave verbs reuse cpu's masked variants. A
  loop over slots split at each verb was rejected because it fails when a
  verb sits under divergent control flow.
- 7.2 **C above 4.** Vulkan vectors stop at 4 components without extra
  capabilities. A C of 8 (64 over 8) was to be carried as two vectors of 4.
  Amended 2026-10-08 on measurement: LLVM's Scalarizer counts its minimum
  fragment in bits, so it keeps an 8-wide boolean mask whole, and SPIR-V
  cannot legalize that. The C lanes are a vector while the slot loop is
  widened, and the emitted SPIR-V carries them as scalars for every C. That
  needs no capability and runs on every device.
- 7.3 **Variants.** For each kernel that pins W, the compiler emits a
  virtual variant for every power of two S from 8 to 2W other than W. For
  W = 32 that is 8, 16 and 64. The SPIR-V of a pinned kernel roughly
  triples, and that is accepted.
- 7.4 **The narrow case is in scope.** It reuses the segmented butterfly.
- 7.5 **The native float reduce order (§6.1).** Decided 2026-10-08: keep the
  driver's reduce and correct workgroup-reduce decision 6.5 and the docs. On
  RADV and amdgpu the driver already sums in the reference butterfly order
  (measured on inputs where a linear sum differs on 110 of 128 waves, pinned
  by `XpuNativeFloatReduceOrderTests`), so no timing leg was run. The plan as
  first written: Measure the butterfly
  against the driver's reduce on RADV and amdgpu first, as an announced
  timing leg. Then decide between correcting decision 6.5 and the docs, and
  moving the native reduce to the butterfly.
- 7.6 **Reporting.** A virtual launch writes nothing to stderr. The launch
  record names the path taken, and tests and tools read it there.
