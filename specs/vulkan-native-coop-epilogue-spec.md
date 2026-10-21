# Vulkan native cooperative-matrix epilogue

## 1. Definition

### 1.1 Purpose
A Vulkan kernel whose cooperative-matrix tiles use an epilogue verb
(`scaledAccumInto`, `rank1Accum` and their variants) is demoted, as a whole,
to the replicated software tile, because the SPIR-V backend has no native
epilogue (`coopMatrixEpilogueSupported()` is false). The replicated tile keeps
every element in every lane. This work gives the Vulkan backend a native path
for the one epilogue shape that SPIR-V can express without a lane-to-element
map: a tile scaled by a scalar and added to another tile.

### 1.2 Why (measured 2026-10-20, gfx1151, RADV mesa 26.0.8)
- `attnFlashPrefillTileKernel` lowers on Vulkan as `software-tile.replicated`.
  RADV reports 256 VGPRs, 2019 spilled, and 493,312 bytes of scratch a wave.
- RADV keeps that scratch for the process once the pipeline has run. The
  prefill route probe (cajeta-llm 15.2.11) runs it once, and the 8B's Vulkan
  peak rises from 10.58 GB to about 11.4 GB against llama.cpp's 10.46 GB.
- The probe measured the replicated tile at 114 ms against 0.8 ms for the
  wave kernels, so Vulkan prefill never uses its matrix cores.
- The kernel's only epilogue use is `sres.scaledAccumInto(sacc, ones,
  broadcast(2^-10))`, which is `sacc += 2^-10 * sres`.

### 1.3 Scope
- A new `CooperativeMatrix` verb for "add a scalar multiple of a tile".
- Its lowering on every tier, native on Vulkan.
- The LLVM fork support the SPIR-V lowering needs.
- The Vulkan device setup cooperative-matrix kernels need.
- The attention tile kernel rewritten on the verb, and measured.

### 1.4 Non-goals
- Per-element epilogues (`get`/`set`/`row`/`col`, row and column factor
  vectors) on Vulkan. SPIR-V defines no lane-to-element map; they stay refused.
- bf16 tiles on Vulkan (no advertised config).
- Changing the route probe (15.2.16.6 stays an unchosen option).

## 2. The verb

- **2.1** When `acc.addScaled(src, s)` runs on two float32 accumulator tiles of
  the same shape and a float32 scalar, every element becomes
  `acc[r][c] + (s * src[r][c])`, rounded after the multiply and after the add.
- **2.2** When the same kernel runs on any tier (native, distributed,
  replicated, cpu, the reference interpreter), the results agree bit for bit.
  No tier contracts the multiply and add into an fma.
- **2.3** When a kernel uses only `addScaled` and no other epilogue verb, its
  tiles are not demoted on a backend without epilogue support.
- **2.4** When `addScaled` is given tiles of different shape, a non-accumulator
  tile, or a non-float32 element type, compilation fails with a diagnostic
  that names the verb.

## 3. SPIR-V lowering

- **3.1** When `addScaled` lowers for Vulkan, it emits
  `OpMatrixTimesScalar` and `OpFAdd` on `OpTypeCooperativeMatrixKHR` values,
  both decorated `NoContraction`, and the module passes `spirv-val`.
- **3.2** When a Vulkan kernel's tiles are all native, its manifest records
  the mma as native, not `software`.
- **3.3** When the LLVM fork lacks the needed intrinsics, the toolchain build
  fails at configure time, not with a silent software tile.

## 4. Device setup

- **4.1** When a Vulkan pipeline holds cooperative-matrix code, it is created
  with full subgroups at the kernel's declared wave width, and the validation
  layer reports no error for it.

## 5. The attention tile

- **5.1** When the 8B prefill runs on Vulkan, `attnFlashPrefillTileKernel`
  needs no scratch, and the process's peak memory is within 1.01× of
  llama.cpp's at ctx 4096, prompt 512.
- **5.2** When the route probe times the native tile against the wave kernels,
  the faster one is used, and Vulkan prefill is no slower than today
  (1204 tok/s at chunk 128, 1437 at 512).
- **5.3** When the kernel runs on hip and nvptx, its output is unchanged from
  today's `scaledAccumInto` form.
