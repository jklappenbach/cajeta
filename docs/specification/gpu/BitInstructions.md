# Per-invocation bit manipulation: `Bits`

`Bits` is the scalar, per-thread bit-twiddling surface. Each work-item operates
on its own 32-bit value, where `Wave.*` is wave-cooperative. There are four
ops, and each is a single hardware instruction on every backend:

```
uint32 r = Bits.reverse(v);        // bit i  <- bit 31-i
uint32 c = Bits.count(v);          // population count, in [0, 32]
uint32 l = Bits.rotateLeft(v, k);  // rotate left  by k (mod 32)
uint32 g = Bits.rotateRight(v, k); // rotate right by k (mod 32)
```

## What it's for

These are the building blocks under hashing, bitset scans, Morton / Z-order
codes, and bit-packing:

- **`count` (popcount)** gives mask cardinality. Pair it with `Wave.ballotSync`
  to count how many lanes satisfy a predicate, or to index a
  popcount-compressed sparse set.
- **`reverse`** serves radix-sort digit ordering, bit-reversal permutations
  (FFT), and building interleaved spatial codes.
- **`rotateLeft` / `rotateRight`** are the inner mixing step of almost every
  integer hash (FNV/Murmur/xxHash-style avalanche).

## How it lowers — every backend, no extension, no fork

These ops are no-patch. They reach the Vulkan/Shader flavor as core SPIR-V via
generic LLVM intrinsics. There is no `llvm.spv.*` fork intrinsic and no SPIR-V
extension to enable.

| Op | Lowering |
|---|---|
| `reverse` | `llvm.bitreverse.i32` → **`OpBitReverse`** / NVPTX `brev` / AMD `v_bfrev` |
| `count` | `llvm.ctpop.i32` → **`OpBitCount`** / NVPTX `popc` / AMD `v_bcnt` |
| `rotateLeft` / `rotateRight` | inline `(v << s) \| (v >> (32-s))`, `s` masked to `[0,31]` → core shifts + `OpBitwiseOr` |

Rotate is expanded inline. The `llvm.fshl`/`llvm.fshr` intrinsics are avoided
because the SPIR-V backend lowers them by emitting a generated helper function
(`spirv.llvm_fsh?_i32`) with external linkage, which pulls in
`OpCapability Linkage`, and `spirv-val` rejects that under Vulkan 1.3.
Expanding the rotate as shift/or keeps everything inline and core-valid. The
mask makes the `s == 0` case a 0-bit shift, so there is no undefined
shift-by-32 and a zero rotate is the identity.

## Caveats

- **32-bit only (v1).** The ops are `uint32 -> uint32`. Vulkan additionally
  restricts `OpBitCount` to 32-bit operands, so the width is a natural stop.
- **`count` returns `[0, 32]`.** The upper bound is inclusive, for the case
  where all bits are set.
- Rotate amounts are taken modulo 32, so `rotateLeft(v, 32) == v`.

---

**Rules.** `Bits.{reverse,count,rotateLeft,rotateRight}` are device-only (inside
an `@Kernel`), `uint32 -> uint32`, one hardware op each, identical on
CPU/Vulkan/AMD/NVIDIA. They need no extension and no fork, only core SPIR-V.
They are bit-exact verified on RADV + gfx1151 + CPU (`XpuBitsDeviceTests`) and
runnable in `samples/tour/gpu` (the `bitOps` kernel). The surface lives in
`runtime/src/cajeta/gpu/core/Bits.cajeta`, and the lowering in
`src/cajeta/xpu/lowering/KernelLowering.cpp`. See `xpu/CajetaXPU.md` for the
kernel surface and `xpu/WaveReductions.md` for the wave-cooperative companion.
