# GPU kernel parity

## 1. Definition

- **1.1** One kernel source runs on every GPU backend cajeta targets, on every device of those
  backends. A developer who writes a GPU kernel never writes it again per device, and never needs
  the device to know it will work:
  - A construct one backend cannot lower is a compiler gap, closed in the compiler.
  - Hardware a device lacks is covered by a fallback that keeps the native operation's result
    contract, and every fallback can be forced and tested on any hardware.
  - A driver or resource limit is a parameter the kernel family adapts to, not a hard-coded size.
  First written for SPIR-V (2026-11-18, sections 2 to 7); widened to every GPU the same day (Julian:
  "Let's go after all the gaps", sections 8 to 12).
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
  - CPU-only kernels (cajeta-llm has 52). Converting an algorithm to a CPU-efficient structure is a
    later spec, once the GPUs are device independent (Julian 2026-11-18).
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

## 8. The remaining compiler gaps

Inventory of 2026-11-18, from cajeta-llm and dev.cajeta.ml.kernels built for every GPU backend on
the dev compiler.

- **8.1** When vulkan runs a kernel as a virtual wave (a device whose subgroup width is not the
  kernel's 32), the kernel lowers. Three causes refused it on 2026-11-18: staged element access was
  not enabled on the virtual target (the f16 attention tiles); slot fission refused a barrier in a
  loop with a lane-dependent trip count, and a barrier in unstructured control flow
  (attnFlashPrefillOnline, gemm256 at f16); and a wave reduction or ballot was left per lane when
  the slot loop did not vectorize (rmsnormRouterTopK, moeGroupWave).
- **8.2** When a portable tile needs a wave on vulkan (`fromWords`, `WaveVector.ofLane`, a type with
  no native matrix config), it is spread across the subgroup, not copied whole into each
  work-item.
- **8.3** When a kernel stores a boolean to a buffer, the SPIR-V is valid. dev.cajeta.ml.kernels'
  `arith_boolean` put OpTypeBool in an externally visible variable, which spirv-val rejects.
- **8.4** When a kernel's `targets` list leaves a backend out only for a gap that has since closed,
  the restriction goes. On 2026-11-18 the seven non-Out16 GEMMs that dev.cajeta.ml.kernels keeps off
  vulkan (gemm128Bf16, gemm256Bf16, gemmWideUnrolled, gemmTiled128Unrolled and its NT, Split and
  SplitTN forms) lowered and passed their value checks there; the four Out16 GEMMs move with
  coop-converting-store.

## 9. The device capability catalog

- **9.1** When host code asks whether the device can do something that changes which code runs, the
  runtime answers from the driver: the matrix configurations it advertises (operand and
  accumulator types and shapes), f64 arithmetic, the subgroup widths it can be pinned to, integer
  dot products, bf16 and f16 storage and arithmetic, and 64-bit atomics. Today `Device.supports`
  knows four capabilities.
- **9.2** When the compiler or the runtime chooses a fallback rung (§10), it reads the same catalog,
  so what the host sees and what runs agree.
- **9.3** When a capability is forced off (§10.4), the catalog reports it off, everywhere.

## 10. Fallbacks for missing hardware

- **10.1** When a device lacks the hardware for an operation, the operation runs on the next rung of
  a ladder that keeps the native operation's result contract. The developer writes the operation
  once and does not name the rung.
- **10.2** When the result contract cannot be kept exactly (f64 on a device without f64, where a
  pair of f32 gives about 49 bits), the rung is refused by name unless the kernel opts in, as
  `@FastMath` gates flushing.
- **10.3** bf16 matrix multiply, the first ladder: a native bf16 matrix unit; else an f16 matrix
  unit with each row of A and column of B scaled by a power of two, exact whenever every scaled
  value is an f16 the bf16 value equals (otherwise that tile takes the next rung); else an f32
  multiply-add tile spread across the subgroup (§8.2). Each rung matches the native operation's
  sums up to accumulation order, which the native operation leaves open.
- **10.4** When a developer or CI forces a capability off on hardware that has it, every kernel takes
  the rung it would take on a device without it, so one machine tests every rung, and the census
  counts each rung that ran.
- **10.5** When a rung is taken, the runtime reports which, per kernel, so a slow path is never
  silent.

## 11. Driver and resource limits

- **11.1** When a kernel's tile, block or shared-memory size is a parameter with listed members, the
  shape filter picks a member that fits the device's limits from the driver, and the tuner times
  the survivors (existing machinery).
- **11.2** When a fixed-size kernel fits no device of a backend it targets, or none of a family's
  members fits, the gate refuses it by name with the limit it exceeds. It is not built and left
  "unavailable" at run time without a word.
- **11.3** cajeta-llm's attnFlashPrefillOnlineKernel (88 KB against gfx1151's 64 KB) and
  dev.cajeta.ml.kernels' convBackWeight3x3 become families whose smaller members fit gfx1151.

## 12. Routes chosen by measurement

- **12.1** When a library picks among routes for one operation (cajeta-llm's int8, Mw, Id and coop
  GEMMs, its f16 column route, its Q8_0 and Q6_K integer defaults), the choice is the tuner's or a
  measured default per device, not a switch on the backend's name. A switch that records a driver
  defect names the defect and a test that shows it.
- **12.2** On 2026-11-18 cajeta-llm held 23 `backendIsVulkan()` and several `hip`, `cuda` and
  `vulkan` name tests. Each is measured on the backends at hand, and kept, replaced or removed on
  the numbers.

