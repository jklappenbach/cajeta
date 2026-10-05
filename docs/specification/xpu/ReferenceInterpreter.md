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
- The survey resolves names that need the compiler's type registries, such as
  enum constants and `@Device` helpers, before anything runs. Those
  registries are thread-local, so a work-item's thread never looks them up.
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
| `Wave.reduceSumF32`, `reduceMaxF32` | an xor butterfly over the full width, partners at distance 1, 2, 4 and up in that order; inactive lanes contribute 0 or -inf. nvptx and cpu sum in this order; amdgpu and SPIR-V use the device's own reduce, whose order is the driver's, so they carry a stated bound |
| `Wave.prefixSum`, `prefixProduct` | exclusive scan in lane order, uint32 |
| `Group.width`, `laneId`, `rowId` | `W`; `linear % W`; `Workgroup.x`, as int32 |
| `Group.reduce(op, v)` | `Wave.reduceSumF32` for `GroupOp.Add`, `Wave.reduceMaxF32` for `GroupOp.Max` |
| `Cajeta.bitsToF32`, `f32ToBits`, `bitsToF64`, `f64ToBits` | the same bits, reinterpreted |
| `Bits.count`, `reverse`, `rotateLeft`, `rotateRight` | on uint32; a rotate takes its amount modulo 32 |
| `for (T k : Group.stripe(n))` | `k` runs from `laneId` while below `n`, stepping by `W` |
| `for (idx, e : buf.range(n))` | `idx` runs from `globalIdX` while below `n`, stepping by the grid's width in work-items; `e` is a copy of `buf[idx]` |
| `Math.min`, `max`, `abs`, `floor`, `ceil`, `trunc`, `fma` | exact, in the argument's float type |
| `Math.sqrt` | correctly rounded in the argument's float type |
| `Math.round` | rounds half away from zero, and returns a float |
| `Math.exp`, `exp2`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `pow`, `rsqrt` | computed in float64, then rounded once to the argument's type. A backend's device library is compared within a stated bound. |
| `buf.vload<N>(i)`, `buf.vstore(i, v)` | lanes `i .. i+N-1`; on a `KernelBuffer` or a `Shared` array |
| `buf.atomicAdd/Sub/Min/Max/And/Or/Xor/Exchange/CompareExchange` | a load, the operation and a store, in work-item order; returns the old value. Integer min and max follow the element's signedness; float min and max are `minnum` and `maxnum`. |
| `v.asUnsigned()`, `asSigned()` | the same bits with the element's signedness flipped |
| `v.asWords()`, `asBytes()` | 8-bit lanes regrouped into int32, little-endian, and back |
| `v.widenLo()`, `widenHi()` | the low or high half of the lanes, each extended by its own signedness to twice the width |
| `v.narrow(w)` | the lanes of `v`, then those of `w`, each truncated to half the width |
| `v.toF32()`, `toF16()`, `toI32()` | lane conversions; `toI32` truncates toward zero |
| `v.bitcastF32()`, `bitcastI32()` | 32-bit lanes reinterpreted |
| `v.lut4(t)` | lane `i` is `t[v[i] & 15]` |
| `v.dotSum(a, acc)` | `acc + Σi v[i]·a[i]`, with `v` extended by its own signedness and `a` sign-extended, wrapping in int32 |
| `v.dot(w)`, `dot(w, acc)` | float: products summed in lane order. 4 × 8-bit integers: both operands take the receiver's signedness. |
| `w.dotAccum(a, acc)` | `acc[j] + Σk w[4j+k]·a[4j+k]`, with `w` extended by its own signedness and `a` always sign-extended, wrapping in int32 |
| a call to a `@Device` helper | a new frame. Arguments are converted to the parameter types, buffers pass by reference, and the result is converted to the return type. A recursive call is refused. |
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
- `WaveVector`, and padded or swizzled `Shared` arrays;
- textures, images and samplers;
- value types.

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

## The conformance corpus

The corpus is real launches, recorded as a backend runs them, then replayed
through the interpreter.

1. Run a program, such as cajeta-llm's tests, with
   `CAJETA_XPU_RECORD=<dir>`. The runtime records the first
   `CAJETA_XPU_RECORD_PER_KERNEL` launches of each kernel (default 2). For
   each one it writes `launch.json`, giving the kernel, backend, grid, block,
   wave width and arguments, plus each allocation an argument points into,
   before (`a<i>.in`) and after (`a<i>.out`). An allocation is written once,
   however many arguments point into it, so aliasing survives the replay.
   The runtime skips a launch on a deferred stream, a Vulkan launch, and one
   whose allocations come to more than `CAJETA_XPU_RECORD_MAX_BYTES`
   (default 64 MiB). Each skip goes in `<dir>/skipped.tsv` with its reason.
2. Compile the same program with `CAJETA_XPU_CONFORMANCE=<dir>`, and
   optionally `CAJETA_XPU_CONFORMANCE_HELD=<file>`. The compiler replays
   every recording against its own kernels, writes `<dir>/conformance.tsv`
   and stops before generating code. It exits 1 when any launch fails.

Each launch gets one of these outcomes:

| Outcome | Meaning | Fails the run |
|---|---|---|
| `pass` | every element agrees | no |
| `fail` | an element disagrees; the detail names the parameter, the element and both values | yes |
| `held` | it disagrees, and the held list tracks it | no |
| `stale` | the held list holds it, but it agrees now | yes |
| `refused` | the interpreter has no semantics for something it uses | no |
| `undefined` | the interpreter met undefined behaviour, or the recording is damaged | yes |
| `missing` | no kernel of that name is in this build | no |

The held list has one tab-separated line per kernel and backend.
`<kernel> <backend> held <note>` holds a known disagreement, and
`<kernel> <backend> ulps=N <why>` states a float bound. A backend of `*`
matches every backend.

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
