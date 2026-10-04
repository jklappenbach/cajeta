# The kernel reference interpreter

The reference interpreter runs a `@Kernel`'s source on the host and produces
the answer every backend is compared against. It reads the same syntax tree
the kernel lowering reads, but none of the lowering's code. Its values carry
their own width and signedness, and its arithmetic follows the host compiler.
A backend that extends with the wrong sign, picks the wrong compare or drops
a lane therefore disagrees with it.

```cpp
#include "cajeta/xpu/reference/KernelInterpreter.h"
namespace ref = cajeta::xpu::reference;

ref::Launch l;
l.grid[0] = 4;
l.block[0] = 64;
ref::run(kernel, {ref::Arg::buffer(y), ref::Arg::buffer(x), ref::Arg::f32(2.5f)}, l);
```

A buffer argument is host memory that the interpreter reads and writes in
place. Its element type is the parameter's `KernelBuffer<T>`. A scalar
argument is passed as its bits.

## Execution model

- Workgroups run one after another, in x, then y, then z order.
- Within a workgroup, each work-item runs until it reaches a rendezvous or
  returns. A rendezvous is `Barrier.workgroup`, `Barrier.wave` or a wave
  collective. Once every live member of the rendezvous's scope has arrived,
  the collective is computed and they all continue.
- Scheduling is deterministic, so a run is reproducible.
- A kernel with no rendezvous runs its work-items in sequence on one thread.
- Memory is sequentially consistent, so `Barrier.workgroupMemory` and
  `Barrier.deviceMemory` order nothing.
- Each `Shared<T>` declaration has one array per workgroup, which every
  work-item of that workgroup binds.
- A work-item's linear index is `x + y·dimX + z·dimX·dimY`.
- Lane and wave come from the linear index and the wave width: the lane is
  `linear % W` and the wave is `linear / W`. Lanes that have returned are
  inactive.
- The wave width is the launch's `waveWidth`. When that is 0, the width comes
  from the kernel's `@Wave(width = N)`. A kernel that uses a wave operation
  with neither is refused, because its answer depends on the width.

## Arithmetic

Arithmetic follows the host compiler's rules (`BinaryOpExpression`).

- An unsuffixed integer literal is `int32`, or `int64` if it is past `int32`
  or has an `L` suffix. Next to a typed operand, a bare literal takes that
  operand's type.
- Integer operands extend to the wider width, each by its own signedness.
  `+`, `-` and `*` wrap.
- `/`, `%` and the bitwise operators are unsigned when either operand is
  unsigned.
- A comparison is signed when either operand is signed.
- `>>` follows the signedness of the shifted operand alone. `>>>` is always
  logical.
- `float32` work is done in float32, one rounding per operation, and nothing
  is contracted into a fused multiply-add.
- `float16` and `bfloat16` are computed in float32 and rounded back, to
  nearest even.
- An `f` suffix makes a literal `float32`. Any other float literal is
  `float64`.
- `!=` on floats is true when either operand is NaN.
- A conversion from float to integer truncates toward zero.

## Built-ins

These are every built-in the interpreter defines.

| Built-in | Reference semantics |
|---|---|
| `KernelThread.x/y/z` | the work-item's index in its workgroup |
| `KernelThread.globalIdX/Y/Z` | `Workgroup.x * dimX + KernelThread.x`, per axis |
| `Workgroup.x/y/z`, `dimX/Y/Z` | the workgroup's index; the block's size |
| `Barrier.workgroup` | every work-item of the workgroup arrives at this barrier before any continues |
| `Barrier.wave` | every live lane of the wave arrives before any continues |
| `Barrier.workgroupMemory`, `deviceMemory` | no effect |
| `Wave.width`, `laneId`, `isFirstLane` | `W`; `linear % W`; `laneId == 0` |
| `Wave.shuffleSync(v, src)` | `v` from lane `src`, which must be active |
| `Wave.rotate(v, d)` | `v` from lane `(laneId + d) % W` |
| `Wave.ballotSync(p)` | bit `i` is set when active lane `i` passes `p` |
| `Wave.reduceSum/Max/Min/And/Or/Xor` | uint32, over the active lanes in lane order; sums wrap |
| `Wave.reduceSumF32`, `reduceMaxF32` | an xor butterfly over the full width; inactive lanes contribute 0 or -inf |
| `Wave.prefixSum`, `prefixProduct` | exclusive scan in lane order, uint32 |
| `Math.min`, `max`, `abs` | exact |
| `buf.vload<N>(i)`, `buf.vstore(i, v)` | lanes `i .. i+N-1` |
| `w.dotAccum(a, acc)` | `acc[j] + Σk w[4j+k]·a[4j+k]`, with `w` extended by its own signedness and `a` always sign-extended, wrapping in int32 |
| `CooperativeMatrix` `splat`, `load`, `store` | element (r, c) is `src[off + r·stride + c]` for row layout 0, or `src[off + c·stride + r]` for column layout 1 |
| `CooperativeMatrix.mma(a, b)` | `this[r][c] += Σk a[r][k]·b[k][c]`, in k order. A float tile accumulates in float32, rounding once per product and once per sum. An integer tile accumulates exactly and wraps. |

A tile is a wave-uniform value. Each work-item holds its own copy, and every
copy is computed the same way from the same arguments.

## What is refused, and what fails

Before any work-item runs, `refusals(kernel)` lists every construct the
interpreter cannot define. Each entry names the construct and its line. If the
list is not empty, `run` throws `XPU-REF01` with the first entry, so the kernel
is never run partially. Today that list includes:

- the epilogue tile verbs, such as `scaledAccumInto` and `fromWords`;
- segmented wave reductions;
- textures, images and samplers;
- atomics;
- `Math` functions beyond `min`, `max` and `abs`, because device
  approximations differ from libm;
- vector methods other than `dotAccum`;
- value types, and `@Device` helper calls.

The conformance corpus adds built-ins here as its kernels need them.

While running, undefined behaviour that the interpreter can see fails the run
with `XPU-REF02`. The message names the operation and its line:

- an index out of bounds;
- a read of a `Shared` element that no work-item wrote;
- a shift by the width or more;
- integer division by zero, or `MIN / -1`;
- a float that converts to an integer out of range;
- a `Barrier.workgroup` that some work-items return without reaching;
- lanes of one wave that reach different wave operations;
- a shuffle from an inactive lane.

## Comparing against it

Integers compare bit for bit. Floats compare within a stated count of units in
the last place, using `ulpDistance`, and each comparison states its bound and
the reason for it.

The interpreter shares the front end with the code under test, so a front-end
defect can agree with itself. cajeta-llm's host oracles stay in that
repository's CI as the independent check on that path.

## Testing the instrument

`CAJETA_XPU_FAULT=dot-activation-sign` restores the single-flag dot lowering
that cajeta 3ba58bda fixed. `XpuReferenceInterpreterTests` sets it, checks
that the cpu backend is caught disagreeing, and checks that the two agree
once it is unset. The variable exists only for that test.

## See also

- `specs/xpu-kernel-independence-spec.md` §2.2 and §7.2.
- `test/xpu/XpuReferenceInterpreterTests.cpp`.
