# Workgroup reduce (spec)

Status: approved by Julian 2026-10-07.

## 1. Definition

### 1.1 Purpose

A kernel that needs one value from every lane of its workgroup, such as the
sum of squares behind an RMSNorm scale or the maximum behind a softmax, has no
verb for it. The reduction verbs stop at one wave: `Wave.reduceSumF32` and the
others reduce the lanes of a wave, and `Group.reduce` is one wave by its own
contract ("one group is one wave"). So a kernel reduces each wave, writes the
wave's partial into a `Shared` array indexed by wave, waits at
`Barrier.workgroup()`, and adds the partials back up by hand:

    float32 wsum = Wave.reduceSumF32(ss);
    if (lane % 32 == 0) { part[lane / 32] = wsum; }
    Barrier.workgroup();
    float32 tot = part[0] + part[1] + part[2] + part[3];

Each copy writes the wave width and the wave count into its source, which is
the thing the verbs exist to keep out of kernels. On a part whose wave or
workgroup is a different width, every copy has to be found and edited.

`Workgroup.reduce` is that operation as a built-in verb.

### 1.2 Who needs it, measured 2026-10-07

cajeta-llm main (40b2b35) reduces across a workgroup by hand in 8 places, and
no other fleet repository does (ml, xgboost, codec, cabra and the stdlib have
none):

- Five RMSNorm sums of squares, over 4 or 8 waves, the result used by every
  lane (`AttnKernel` and `QuantKernel`).
- One softmax maximum over 8 waves, used by every lane (`AttnKernel`).
- One MoE router logit sum over 4 waves, written by one lane (`QuantKernel`).
- One segmented flash-attention partial, which combines per segment rather
  than across the workgroup (`AttnKernel`).

The first seven are this spec's shape. The eighth is a different operation
and stays as it is (1.4).

### 1.3 Constraints

- Every backend lowers the verb correctly: nvptx, amdgpu, vulkan and cpu,
  and the reference interpreter models it.
- A result is the same on every backend, bit for bit, because the combine
  order is defined (6.3).
- A kernel's source names no wave width and no wave count to use it.

### 1.4 Non-goals

- A segmented workgroup reduce. The flash-attention site keeps its own code.
- Reductions across workgroups (a grid reduce). Kernels that need one keep
  using atomics or a second launch.
- A faster lowering than the hand-written form. The verb must not be slower
  (5.1). A hardware path is a later change, and it may not change the order.

## 2. The verb

### 2.1 Requirements

`Workgroup.reduce(GroupOp op, T value)` combines `value` from every lane of
the calling workgroup and returns the result to every lane. `T` is `float32`
or `int32`. `op` is a compile-time `GroupOp`: `Add`, `Max` or `Min`. `GroupOp`
gains `Min`, appended so the existing ordinals do not move, and `Group.reduce`
accepts the same operators and types, so one enum means the same thing in both.

The verb is a barrier: every lane of the workgroup reaches it, and none
continues until all have contributed. Its scratch storage is the compiler's,
not the kernel's.

### 2.2 Use cases

- **2.2.1** When every lane of a workgroup calls `Workgroup.reduce(GroupOp.Add,
  x)`, every lane receives the sum of all the lanes' `x`.
- **2.2.2** When the operator is `Max` or `Min`, every lane receives the
  maximum or minimum.
- **2.2.3** When the value is an `int32`, the result is the exact integer sum,
  maximum or minimum, wrapping as `int32` addition wraps on the target.
- **2.2.4** When the workgroup holds one wave, the result equals
  `Group.reduce` of the same value.
- **2.2.5** When a kernel's launch block is not a compile-time constant, the
  verb still covers every wave the launch holds, up to the kernel's declared
  `@Occupancy(maxThreads)` ceiling or the part's limit.
- **2.2.6** When a kernel calls the verb twice in a row, the second call reads
  no partial left by the first.

## 3. Order and results

### 3.1 Requirements

Each wave reduces its lanes as `Group.reduce` does. The per-wave partials then
combine left to right in wave order, wave 0 first, the order the hand-written
sites use. The result of a `float32` reduction is therefore one defined value,
identical on every backend and in the reference interpreter.

### 3.2 Use cases

- **3.2.1** When the same kernel runs on two backends with the same inputs,
  the two results are bit-identical.
- **3.2.2** When a hand-written site that combined `part[0] + part[1] + ...`
  is replaced by the verb, its output is bit-identical to before.
- **3.2.3** When the reference interpreter replays a recorded launch that uses
  the verb, it produces the device's result exactly.

## 4. Refusals

### 4.1 Requirements

Misuse is a compile error that names the verb and the cause, never a hang or
a wrong value at run time.

### 4.2 Use cases

- **4.2.1** When the verb sits under control flow that can diverge between the
  lanes of a workgroup, the kernel is refused, as a divergent barrier is.
- **4.2.2** When `op` is not a literal `GroupOp`, the kernel is refused.
- **4.2.3** When `value` is a type other than `float32` or `int32`, the
  kernel is refused, naming the type.
- **4.2.4** When the verb is called from host code, the call is refused, as
  the other kernel verbs are.

## 5. Adoption in cajeta-llm

### 5.1 Requirements

Once a compiler release carries the verb, cajeta-llm's seven whole-workgroup
sites move to it. Each moved kernel's output stays bit-identical, its time is
no worse than the hand-written form at the engine's shapes, and its source no
longer names a wave width or a wave count.

### 5.2 Use cases

- **5.2.1** When the seven sites use the verb, cajeta-llm's suites on cpu,
  amdgpu and nvptx pass with the census clean.
- **5.2.2** When a moved kernel is timed against its hand-written form at the
  engine's shapes, it is no slower.

## 6. Decisions

- **6.1** (Julian 2026-10-07) The verb is `Workgroup.reduce`, in a new
  `Workgroup` class beside `Barrier.workgroup()`. `Group` stays one wave, as
  its contract says.
- **6.2** (Julian 2026-10-07) Every lane receives the result. There is no
  first-lane variant.
- **6.3** (Julian 2026-10-07) The combine order is fixed: each wave reduces,
  then the partials combine from wave 0 upward.
- **6.4** (Julian 2026-10-07) The work covers the verb for `Add`, `Max` and
  `Min` over `float32` and `int32` on every backend, the reference
  interpreter and the conformance corpus, a release, and cajeta-llm's seven
  sites. The segmented flash-attention site stays as it is.
