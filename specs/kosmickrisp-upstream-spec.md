# KosmicKrisp upstream contributions — spec

> Status: **draft**. Evidence: `apple-vulkan-findings.md` §4. Plan:
> `agents/kosmickrisp-upstream-plan.md`. Work happens in `~/code/mesa`
> (`jklappenbach/mesa` fork); artifacts that do not belong in Mesa live here.
>
> **Nothing is pushed anywhere without Julian's explicit say-so, per action.**

## 1. Definition

### 1.1 Purpose
Close the two capability gaps that make Apple a second-class xpu target:
`VK_KHR_cooperative_matrix` and `VK_KHR_shader_atomic_int64`. KosmicKrisp, not
MoltenVK — Mesa already owns the NIR infrastructure both need, and MoltenVK's
coop-matrix work is someone else's and stalled (findings §4.1).

### 1.2 Why this is tractable off Apple hardware
`libmsl_compiler` (`src/kosmickrisp/compiler/`, 5,137 lines) depends only on
`idep_nir`, `idep_mesautil` and `idep_vulkan_runtime`. **No Metal, no
Objective-C** — that is confined to `bridge/`. `nir_to_msl()` is `nir_shader*`
in, MSL text out. So the compiler half builds and tests on Linux; only running
the result needs a Mac.

### 1.3 Scope
- `VK_KHR_cooperative_matrix` on KosmicKrisp via MSL `simdgroup_matrix`.
- `VK_KHR_shader_atomic_int64` on KosmicKrisp, **gated on an experiment**.
- The `docs/` and `features.txt` changes Mesa requires alongside them.

### 1.4 Non-goals
- **MoltenVK cooperative matrix.** SPIRV-Cross merged MSL codegen (PR #2596);
  driver PR #2753 is open and WIP-blocked, and it is zainsharief's. Leave it.
- **Metal 4 `tensor_ops::matmul2d`.** The full-coverage target, on which neither
  Mesa nor SPIRV-Cross has written a line. Separate, larger, later.
- **KosmicKrisp on iOS.** Third gap, unrelated machinery.
- **Opening any MR.** Explicitly out of scope until Julian says so.

### 1.5 What this buys cajeta — state it plainly
`simdgroup_matrix<T,8,8>` takes **one** type parameter for all four operands and
has no integer types. So of cajeta's five GEMM kernels it covers **two**:

| kernel | covered |
|---|---|
| `Ewise.matmulF32` (f32→f32) | yes |
| `Ewise.matmulBf16` (bf16→bf16) | yes |
| `matmulF16` (f16→f32) | no — mixed types unrepresentable |
| `matmulBf16Wide` (bf16→f32) | no — same |
| `matmulI8` (i8→i32) | no — no integer matrices |

The mixed-precision kernels that LLM inference actually uses are **not**
reachable this way. That is a Metal 4 tensor problem, not a lowering bug.

## 2. Cooperative matrix

- **2.1** When a compute shader declares `CooperativeMatrixKHR` on an Apple7+
  device, the shader compiles to MSL that uses `simdgroup_matrix`.
- **2.2** When the driver reports supported configurations, it reports exactly
  the ones MSL can express: M=N=K=8, subgroup scope, A/B/C/Result all the same
  type, for `float16`, `bfloat16` and `float32`.
- **2.3** When a shader uses a 16×16×16 (or other) shape, Mesa's
  `nir_lower_cooperative_matrix_flexible_dimensions()` decomposes it to 8×8×8
  granules before the MSL backend sees it.
- **2.4** When lowering, the matrix stays **opaque** — an MSL `simdgroup_matrix`
  value, never scalarized. panvk/radv/anv scalarize because they own a register
  allocator; KosmicKrisp emits source text and must not. `spirv_msl.cpp` is the
  model.
- **2.5** When a shader uses per-element access (`cmat_extract`, `cmat_insert`,
  `cmat_length`, `cmat_get_coordinate`), the behaviour is defined or the
  configuration is not advertised — MSL states the element-to-lane mapping is
  *unspecified*, so this cannot be guessed.
- **2.6** When the device is below Apple7, the extension is not advertised.
- **2.7** When the CTS `dEQP-VK.compute.cooperative_matrix.*` group runs, it
  passes for the advertised configurations.

## 3. 64-bit atomics

- **3.1** Before any driver change, the experiment in §4 settles what Metal
  actually supports. Apple's MSL spec and its Feature Set Tables contradict each
  other; this is empirical, not a documentation question.
- **3.2** When the experiment shows the full atomic set on Apple9+,
  `KHR_shader_atomic_int64` is advertised there with `shaderBufferInt64Atomics`.
- **3.3** When it shows only `min`/`max` returning void, the extension stays
  false and the finding is written up — `OpAtomicUMin` must return the previous
  value, which a void-returning intrinsic cannot provide.
- **3.4** When advertised, it is gated on the measured family, never on the
  documentation.

## 4. Validation

No Apple hardware. The split:

- **4.1** The MSL the backend emits is asserted **on Linux**, from SPIR-V in to
  text out, with no Metal present.
- **4.2** Whether that MSL compiles and runs is a Mac question — the rented host
  (`apple-vulkan-spec.md` §7.3).
- **4.3** The 64-bit atomic experiment (`specs/schemas/metal-atomic64-probe.swift`)
  needs an M3/M4 and blocks all of §3.
- **4.4** CTS is a Mac question.

## 5. Mesa house rules

- Trailer is `Assisted-by:`, not `Co-authored-by:`.
- Docs: `docs/drivers/kosmickrisp.rst`, `docs/features.txt`,
  `docs/relnotes/new_features.txt`.
- Scale reference: panvk's equivalent was MR !42723, +767 lines, merged
  2026-08-05. KosmicKrisp's is ~700–1,200 lines with no ISA or RA work.

## 7. The MSL surface — confirmed 2026-09-02

From shipping Metal code (`llama.cpp/ggml-metal.metal`) and SPIRV-Cross's
`spirv_msl.cpp`, the API the lowering targets is:

| NIR | MSL |
|---|---|
| `cmat_construct` | `make_filled_simdgroup_matrix<T, 8>(scalar)` |
| `cmat_load` | `simdgroup_load(dst, ptr, stride, offset, transpose)` |
| `cmat_store` | `simdgroup_store(src, ptr, stride, offset, transpose)` |
| `cmat_muladd` | `simdgroup_multiply_accumulate(D, A, B, C)` |
| `cmat_length` | `sizeof(simdgroup_matrix<T,8,8>::storage_type) / sizeof(T)` |

## 6. Open questions

- **6.1** Per-element access. **Partly resolved.** SPIRV-Cross does not implement
  it: its `default:` arm throws *"Unsupported operation on cooperative matrix in
  MSL backend"*, and it throws again specifically on `OpCompositeExtract` /
  `OpVectorExtractDynamic` from a coopmat. That is why four of four real
  coop-matrix programs failed on MoltenVK.
  **Still open, and it is the interesting question:** MSL's `simdgroup_matrix`
  is reported to expose `thread_elements()`, and `VK_KHR_cooperative_matrix`
  leaves the element-to-lane mapping *unspecified too* — it only requires that
  an invocation sees a consistent slice. If both hold, element access IS
  implementable via `thread_elements()`, SPIRV-Cross simply never did it, and
  KosmicKrisp could be strictly better than MoltenVK here. **Unverified — no
  Apple MSL headers on this machine.** Check `metal_simdgroup_matrix` on the
  Mac before deciding to refuse.
  `cmat_get_coordinate` is different: it needs the real (row, col), which MSL
  genuinely does not give. That one is refusable — it is not base KHR.
- **6.2** Is `bfloat` available on all Apple7+, or only Apple8+? Gates 2.2.
- **6.3** Does the M5 Neural Accelerator engage through `simdgroup_matrix`, or
  only through Metal 4 tensors? Apple states nothing either way. If only the
  latter, this work never collects the M5 uplift.
