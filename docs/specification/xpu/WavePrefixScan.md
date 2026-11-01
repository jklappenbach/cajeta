# Wave prefix scans: `Wave.prefixSum` / `Wave.prefixProduct`

An **exclusive** prefix scan turns one `uint32` per lane into a per-lane partial.
Lane `i` receives the sum (or product) of the values from lanes `0..i-1`, and lane 0
receives the identity (`0` for sum, `1` for product).

```
uint32 offset = Wave.prefixSum(count);   // where my run starts within the wave
uint32 pp     = Wave.prefixProduct(x);   // exclusive running product
```

## What it's for

The exclusive prefix sum is the workhorse of lane compaction and allocation.
Each lane computes how many items it wants to emit, and `prefixSum` gives it the
exact slot offset where its items go, with no atomics and no contention.

- **KernelStream compaction.** Keep the lanes that pass a predicate, packed: `slot =
  prefixSum(keep ? 1 : 0)`.
- **Per-wave allocation.** Sub-allocate from a shared buffer by reserving
  `reduceSum(n)` once and indexing with `prefixSum(n)`.
- **Sorting / histogram.** The per-digit offset step of a radix sort.

`prefixSum(1)` per lane yields the lane's index within the wave. That is the scan's
"hello world".

## How it lowers — native on Vulkan, a portable scan elsewhere

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | a single native **`OpGroupNonUniformIAdd` / `OpGroupNonUniformIMul`** with the **ExclusiveScan** group operation (the fork `llvm.spv.wave.prefix.{sum,product}` intrinsics). |
| **AMD** | a Hillis-Steele log-step scan over **`ds_bpermute`** (the divergent intra-wave gather). |
| **NVIDIA** | the same Hillis-Steele scan over **`shfl`**. |
| **CPU** | an in-lane exclusive scan in the kernel's vectorized VFABI variant. |

Only Vulkan exposes a single-instruction subgroup scan. AMD and NVIDIA have no
scan intrinsic, only the shuffle primitives, so the portable default builds the
scan from `log2(width)` shuffle steps. Cajeta carries the scan on a `waveScan`
seam. The base default is the Hillis-Steele algorithm built on a new
`waveShuffleDivergent` seam. AMD overrides that seam to `ds_bpermute`, since its normal
`shuffleSync` is `readlane`, which needs a uniform source lane. Vulkan
overrides `waveScan` to the native op, and CPU overrides it to the VFABI variant.

The fork addition is only the Vulkan reach, which is the `llvm.spv.wave.prefix.*`
intrinsics and their GlobalISel selection, carried on `cajeta-spirv`. There is no in-tree
producer, so it is not an upstream PR. Everything else is portable cajeta lowering.

## Caveats

- **Exclusive (not inclusive).** Lane `i` excludes its own value. For the
  inclusive scan, add your own value back: `prefixSum(x) + x`.
- **`uint32` (v1), wraps on overflow.** Sum/product are modulo 2³².
- **Cross-lane → maximal reconvergence.** Like the other cross-lane ops, a
  scanning kernel requests maximal reconvergence on Vulkan
  (see `CajetaXPU.md §6.3`).

---

**Rules.** `Wave.prefixSum(uint32) -> uint32` and `Wave.prefixProduct(uint32) ->
uint32` are device-only, wave-cooperative exclusive scans (lane 0 = identity).
Vulkan uses the native `OpGroupNonUniform … ExclusiveScan` through a `cajeta-spirv` fork
intrinsic. AMD/NVIDIA use a `ds_bpermute` / `shfl` Hillis-Steele scan, and CPU uses a VFABI
scan. The ops are device-verified bit-exact on RADV (native) and gfx1151 (ds_bpermute)
(`XpuWaveDeviceTests.*PrefixScan*`), and CPU is exercised in `samples/tour/gpu`
(`wavePrefix`). See `WaveReductions.md` for the reduce family and `Wave`
(`runtime/.../core/Wave.cajeta`) for the rest of the surface.
