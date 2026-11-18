# SPIR-V kernel parity

## 1. Definition

- **1.1** One kernel source runs on every GPU backend cajeta targets. A kernel that lowers for
  amdgpu and nvptx lowers for vulkan, at the same results, with no per-backend copy and no
  `targets` restriction written to get around the SPIR-V lowering.
- **1.2** Why. On 2026-11-18 (cajeta-llm 15.2.21.9.4, proton, Gemma 3 4B Q4_K_M) Vulkan decode
  was at llama.cpp's level (98% at depth 512, 105% at 1536) and prefill was not (60% at 512, 38%
  at 1536). The kernels that carry hip prefill have no SPIR-V:
  - 17 llm kernels are held off vulkan: the f16 flash prefill tiles, five matrix-core mat-vec
    kernels (xpu-kernel-adaptor 3.2.6) and eight int8 tiles (xpu-tile-shape-selection 4.9.1).
  - Four f16 column kernels were restricted to amdgpu and nvptx on 2026-11-18 to get a Vulkan
    build at all.
  - The 16-bit-output GEMMs in dev.cajeta.ml.kernels (cajeta-ml-gpu 23.2.16) carry a target
    restriction and a Vulkan-only fallback.

  Vulkan then runs the older routes: the score and combine attention pair, and the f16
  cooperative GEMM with a conversion per projection.
- **1.3** The causes are in cajeta's SPIR-V lowering, not in SPIR-V:
  - vectors wider than four lanes;
  - `Shared<T>` parameters on `@Device` helpers;
  - per-element access to a native cooperative-matrix tile, which has no lane map;
  - `fromWords` and `WaveVector.ofLane` on a tile that is copied whole into each work-item
    instead of spread across the wave.
- **1.4** Constraints:
  - Use only Khronos extensions that the drivers ship: SPV_KHR_cooperative_matrix and
    subgroup-size control. SPIR-V is not extended.
  - Correctness never depends on a guessed lane-to-element map.
  - A probe dispatch stays under 1 s on proton.
- **1.5** Non-goals:
  - Tuning Vulkan prefill to llama.cpp's level. That is cajeta-llm 15.2.21.9.4.1 and .9.4.2, once
    these kernels lower.
  - MoltenVK and Windows drivers past what already runs there.

## 2. Vectors wider than four lanes

- **2.1** When a kernel builds a `Vector<T, N>` with N of 8 or 16 (a 16-bit type at 8 lanes, an
  8-bit type at 16), it lowers on vulkan and gives the cpu backend's results.
- **2.2** When such a vector is loaded, bitcast, converted or reduced, each step lowers the same
  way. That covers `bitcastF16`, `asHalf`, `dotSum` and lane extraction.
- **2.3** When cajeta-llm's four f16 column kernels build for vulkan with their `targets`
  restriction removed, the build passes.

## 3. Shared memory passed to helpers

- **3.1** When a `@Device` helper takes a `Shared<T>` parameter, the kernel that calls it lowers
  on vulkan and gives the same results as on amdgpu.
- **3.2** When the helper is called from more than one site, with different `Shared<T>` arrays,
  each site reads and writes its own array.

## 4. Element access on a native tile

- **4.1** When a kernel calls `elements`, `get`, `set`, `row` or `col` on a native SPIR-V
  cooperative-matrix tile, it lowers. Each element has the (row, col) that the kernel source
  computes on amdgpu and nvptx, for every shape, type and use the native tier takes.
- **4.2** When the lowering cannot read the driver's own layout, it goes through workgroup memory.
  It stores the tile, synchronizes the subgroup, and serves each element access there (`set`
  writes back before the tile is next used). This works on every driver.
- **4.3** When a probe at device open has identified the driver's layout for a shape, type and use,
  the kernel reads the elements in registers. The result is the same as 4.2's, bit for bit.
- **4.4** When the probe finds a layout it does not recognize, or the probe cannot run, the kernel
  takes 4.2's path, and the runtime reports which path each shape took.

## 5. Tiles built from lanes

- **5.1** When a kernel builds a tile with `fromWords`, or reads `WaveVector.ofLane`, it lowers on
  vulkan for both tiers, the native one and the portable one.
- **5.2** When the portable tier serves such a tile on vulkan, the tile is spread across the
  subgroup, whose width the runtime pins (32 where subgroup-size control is usable).
- **5.3** When subgroup-size control is not usable and the width is not the one the tile needs, the
  kernel is refused by name at load. It never runs on a guessed width.

## 6. Holds and workarounds

- **6.1** When this work is done, no kernel in cajeta-llm or dev.cajeta.ml.kernels is held off
  vulkan for a reason in §2 to §5. The `targets` restrictions and the Vulkan-only fallback named in
  1.2 are gone.
- **6.2** When a cajeta-llm change stops a kernel lowering for vulkan, CI fails at push. A Vulkan
  build runs in the llm CI. It is not a test run, so it never drives the gfx ring.
- **6.3** When a build holds kernels off a backend, the build ends with a count per backend and
  per tracking item, so a hold is never only a note in the middle of the log.

## 7. Measures

- **7.1** When the holds lift, cajeta-llm's Vulkan suite passes the tests of every kernel they
  covered, and its census has no uncovered rows on vulkan.
- **7.2** When the holds lift, 15.2.21.9.4's comparison is taken again the same hour as
  llama.cpp. Per-phase prefill times are recorded against the 2026-11-18 numbers: attend 130 ms at
  512 and 967 ms at 1536.
