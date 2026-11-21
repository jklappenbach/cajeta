# coop-stated-alignment — a kernel states its tile alignment, and nvptx drops the placement guard

Plan: [coop-stated-alignment-plan](../../agents/archive/coop-stated-alignment-plan.md).
Decided with Julian 2026-11-20: the author states alignment with `align:`, as `vload` and
`vstore` already do, and a `--debug` build traps on a wrong statement. Approved 2026-11-20.

## 1. Definition

Since 0.54.0, every native `CooperativeMatrix` load, store and `storeAs` on nvptx carries a
run-time placement guard (coop-converting-store Unit 5, gpu-kernel-parity §10.4). wmma
moves each lane's contiguous run as one access, so it needs the tile's address and its row
pitch to be multiples of that run: 8 bytes for a float32 or int32 accumulator, 4 for a
16-bit fragment or an 8-bit A operand, 1 for the row-major u8 B operand. Off that, a tile
faults or writes the wrong cells. The guard tests the address and pitch at run time, takes
wmma when they are aligned, and otherwise gathers or scatters per element.

The guard made those tiles right at any placement, and it costs registers, because each
site now carries both paths. On sm_89 two cajeta-llm kernels began to spill:
`WmmaKernel.q4kWmmaDeqMw8Kernel` (8 bytes) and `q5kWmmaIdMwKernel` (24 bytes). With the
guard switched off by a probe they do not spill (2026-11-20). Their tiles are always
aligned. The kernel knows that and the compiler cannot see it, because the offsets are
computed at run time.

## 2. Features

### 2.1 A load or store states its alignment

`load`, `store` and `storeAs` on a `CooperativeMatrix` take an optional trailing `align:`,
a constant power of two from 1 to 16 bytes, as `vload` and `vstore` do:

```cajeta
acc.storeAs<float16>(y, offset, 0, stride, align: 16);
a.load(sa, offset, 0, 72, align: 16);
```

It states that both the tile's starting address and its row pitch (the stride in bytes)
are multiples of `align` bytes. Where the stated alignment covers the fragment's run, nvptx
emits the wmma form alone, with no guard and no per-element path. Where it does not cover
the run, the statement changes nothing and the guard stays. The other backends need no
placement guard and accept `align:` without effect.

Use cases:
1. cajeta-llm's wmma kernels state `align: 16` and go back to one path, with no spill on sm_89.
2. A GEMM whose shared-memory tiles sit at padded, aligned offsets states it and keeps its
   registers.
3. A kernel that cannot promise alignment says nothing and stays right at any placement.

### 2.2 The compiler proves what it can

Where the address and pitch are provably aligned from constants, as in a shared tile at a
constant offset with a constant stride, the guard folds away without a statement. This is
the optimizer's existing constant folding, and the spec records it so a kernel author
knows a statement is only needed for run-time offsets.

Use cases:
1. A tile at `sa` offset 0 with stride 72 halves needs no `align:`.

### 2.3 A wrong statement is caught where it can be

A stated alignment the address does not meet is undefined on nvptx, as with `vload`: the
tile faults or writes wrong cells. A `--debug` build (ubTraps on, as for `vload`'s
statement) verifies each stated alignment at run time. A violation is recorded in the
kernel's fault record and the access is skipped, so a misaligned wmma never faults the
context, and the launch fails with a named error giving the kernel, the line, the stated
alignment and the misaligned address or pitch. A test run finds a wrong statement before
a release build hides it. Vulkan is not checked: its logical pointers have no address,
and it has no guard to drop.

Use cases:
1. A test that states `align: 16` on a tile at offset 2 halves fails in `--debug` with an
   error naming the kernel and the misaligned address.
2. A release build of the same kernel emits no check.
