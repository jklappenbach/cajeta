# Per-invocation bit manipulation: `Bits`

`Bits` is the scalar, per-thread bit-twiddling surface — each work-item operates
on its own 32-bit value (contrast `Wave.*`, which is wave-cooperative). Four
ops, each a single hardware instruction on every backend:

```
uint32 r = Bits.reverse(v);        // bit i  <- bit 31-i
uint32 c = Bits.count(v);          // population count, in [0, 32]
uint32 l = Bits.rotateLeft(v, k);  // rotate left  by k (mod 32)
uint32 g = Bits.rotateRight(v, k); // rotate right by k (mod 32)
```

## What it's for

The building blocks under hashing, bitset scans, Morton / Z-order codes, and
bit-packing:

- **`count` (popcount)** — mask cardinality. Pair with `Wave.ballotSync` to
  count how many lanes satisfy a predicate, or to index a popcount-compressed
  sparse set.
- **`reverse`** — radix-sort digit ordering, bit-reversal permutations (FFT),
  and building interleaved spatial codes.
- **`rotateLeft` / `rotateRight`** — the inner mixing step of almost every
  integer hash (FNV/Murmur/xxHash-style avalanche).

## How it lowers — every backend, no extension, no fork

These are **no-patch**: they reach the Vulkan/Shader flavor as *core* SPIR-V via
generic LLVM intrinsics — there is no `llvm.spv.*` fork intrinsic and no SPIR-V
extension to enable.

| Op | Lowering |
|---|---|
| `reverse` | `llvm.bitreverse.i32` → **`OpBitReverse`** / NVPTX `brev` / AMD `v_bfrev` |
| `count` | `llvm.ctpop.i32` → **`OpBitCount`** / NVPTX `popc` / AMD `v_bcnt` |
| `rotateLeft` / `rotateRight` | inline `(v << s) \| (v >> (32-s))`, `s` masked to `[0,31]` → core shifts + `OpBitwiseOr` |

**Why rotate is expanded inline, not `llvm.fshl`/`llvm.fshr`.** The SPIR-V
backend lowers the funnel-shift intrinsics by emitting a *generated helper
function* (`spirv.llvm_fsh?_i32`) with external linkage — which pulls in
`OpCapability Linkage`, rejected by `spirv-val` under Vulkan 1.3. Expanding the
rotate as shift/or keeps everything inline and core-valid. The mask makes the
`s == 0` case a 0-bit shift (no undefined shift-by-32) so a zero rotate is the
identity.

## Caveats

- **32-bit only (v1).** The ops are `uint32 -> uint32`. Vulkan additionally
  restricts `OpBitCount` to 32-bit operands, so the width is a natural stop.
- **`count` returns `[0, 32]`** — note the inclusive upper bound (all bits set).
- Rotate amounts are taken **modulo 32**; `rotateLeft(v, 32) == v`.

---

**Rules.** `Bits.{reverse,count,rotateLeft,rotateRight}` are device-only (inside
an `@Kernel`), `uint32 -> uint32`, one hardware op each, identical on
CPU/Vulkan/AMD/NVIDIA. No extension, no fork — core SPIR-V. Bit-exact verified on
RADV + gfx1151 + CPU (`XpuBitsDeviceTests`). Runnable in `samples/Tour/xpu`
(the `bit ops` section). See `CajetaXPU.md` for the kernel surface and `Wave`
(`cajeta-docs/`) for the wave-cooperative companion.
