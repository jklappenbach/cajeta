# gpu-f16-torch-parity — spec

## 1. Definition

### 1.1 Purpose & scope
Close the remaining gap between Cajeta's shipped f16 GEMM (26.2 TFLOP/s @ n2048,
gfx1151) and torch/hipBLASLt's verified 38.2 TFLOP/s on the same device, by
replicating the techniques the torch-dispatched Tensile kernel actually uses —
established from the Tensile source, not inferred. Scope is the full set of
techniques in that kernel's recipe: block-granular LDS padding, two-stage global
prefetch (PGR2), and wide (256-bit-equivalent) LDS reads (LRVW16), plus any
instruction-scheduling that those unlock. amdgpu-only flavor (CPU barrier-fission
OOMs in a K-loop; out of scope here).

### 1.2 Problem statement
The shipped kernel pads LDS **per row** (depthU 64 → physical stride 66, odd
half-stride 33). rocprof showed this drove LDS bank conflicts 84% → 38%
(11.9 → 26.2 TFLOP/s). The residual 38% is the **transposed-B staging writes**:
per-row padding moves the pad boundary to coincide with the *read* stride, so it
de-conflicts reads but the transposed write stride never crosses a row boundary and
keeps colliding. This is the binding constraint for the write-side conflict
component of the 26→38 gap.

### 1.3 Ground truth — what torch actually does
The torch-dispatched kernel (verified via torch.profiler) is Tensile solution
`Cijk_Ailk_Bljk_HHS_..._MT96x128x64_MI16x16x1_GRVWA8_GRVWB8_LRVW16_MIWT3_4_PGR2_DTLB0_ISA1151_WG32_4_1`.
From the Tensile source (`ml/TheRock/rocm-libraries/projects/hipblaslt/tensilelite/Tensile`):

- **Block-granular additive LDS padding (the crux).** The physical LDS offset is
  computed identically at global-load staging and at WMMA fragment read
  (`LocalRead.py:314,835`; `KernelWriterAssembly.py:3187,3254`):

  ```
  physical_offset = logical_offset + (logical_offset / LdsBlockSizePerPad) * LdsPad * bpe
  ```

  i.e. insert `LdsPad` elements after every `LdsBlockSizePerPad` *bytes* — a fixed
  **byte-period** independent of the logical row/col structure. There is **no XOR
  swizzle** anywhere in the LDS address path; it is purely additive padding. Because
  the same formula governs both the write address and the read address, one physical
  layout serves both, with no separate strides. The byte-granular block boundary (vs
  our row-granular boundary) is what shifts the bank mapping for the *write* stride as
  well as the *read* stride.

- **LRVW16 wide LDS reads.** On RDNA3 `ds_read` caps at `b128`; LRVW16 (16 f16 =
  256 bits) lowers to **two `ds_read_b128`** with two offsets (`na=2`), both into the
  padded layout (`LocalRead.py:863`, `KernelWriter.py:3987`).

- **PGR2 two-stage prefetch.** Ping-pong over **2 LDS buffers**; prologue prefetches
  iterations 0 and 1, steady state computes iteration N-1 while loading N, epilogue
  drains (`KernelWriterAssembly.py:5608-5617`).

### 1.4 Constraints
- **GPU-free-probe-first.** Every layout/scheduling change is first validated by an
  ISA probe (read/write access widths via `ds_read_b128`/`ds_store_b128` counts,
  spills via `.vgpr_spill_count`) and, where possible, a modeled bank-conflict count,
  before any on-device run. Same harness as the prior effort.
- **rocprof for the real conflict %.** On-device confirmation via
  `100*SQC_LDS_BANK_CONFLICT/SQC_LDS_IDX_ACTIVE` on a quiet GPU.
- **Honest measure-ship-or-record.** Every throughput claim measured at n2048 vs the
  committed 26.2 baseline and vs torch 38.2; ship only a measured win, never a
  regression; record negative results.
- **amdgpu-only flavor** (CPU barrier-fission OOMs on barrier-in-K-loop).

### 1.5 Non-goals
- **No XOR / CUTLASS-style swizzle.** Retracted: the prior "fragment-geometry swizzle"
  framing was based on a wrong model; torch uses additive block padding. Element-
  granular `Swizzled<T,S>` scalarizes wide reads and is not the mechanism here.
- bf16 / int8 / fp8 datatypes (f16 only this effort).
- Tile shapes other than the torch 96×128×64 / MI16×16×16 recipe.
- CPU `@Kernel` flavor.

## 2. Block-granular additive LDS padding

The single physical LDS layout that is simultaneously wide-writable from staging and
wide-readable by the WMMA fragment, with no separate strides, via a fixed byte-period
additive pad applied identically to both the staging-write address and the
fragment-read address.

### Requirements
- The compiler must support an LDS tile whose physical offset is
  `logical + (logical / LBSPP_bytes) * LdsPad_elems` for a configurable
  `(LdsBlockSizePerPad, LdsPad)`, applied at **both** the staging store address and
  the cooperative-matrix fragment-read address (`fragCoord`), so a value written by
  staging is read back correctly by the WMMA load (round-trip identity).
- Wide accesses must survive: the staging store stays `ds_store_b128` and the fragment
  read stays `ds_read_b128` (block padding shifts whole 128-bit chunks; it must not
  scalarize either side, unlike element-swizzle).
- `(LdsBlockSizePerPad, LdsPad)` must be expressible as a property of the shared tile
  (general, reusable across WMMA GEMM kernels), not hardcoded into one kernel body.

### Use cases
- **2.1** As the GEMM kernel, when I declare a block-padded shared B tile and stage a
  128-bit global panel into it, then the staging emits `ds_store_b128` at the padded
  address and the round-trip read returns the written values (single-thread CPU/JIT
  oracle).
- **2.2** As the compiler, when I lower a cooperative-matrix `.load` from a block-padded
  tile, then `fragCoord` applies the same additive-pad formula and the read emits
  `ds_read_b128` (ISA probe: `ds_read_b128 > 0`, `ds_read_u16 == 0`, no `Cannot select`).
- **2.3** As a developer sweeping `(LBSPP, LdsPad)`, when I pick the torch values for
  the 96×128×64 geometry, then modeled/measured LDS bank conflicts for **both** the
  write and the read access patterns fall below the 38% the row-pad baseline leaves
  (rocprof on-device).
- **2.4** As the existing `Swizzled<T,S>` users, when block padding lands, then the
  existing element-swizzle tests still pass (the new path is additive padding, gated
  separately, not a change to the XOR path).

## 3. Compiler surface — where padding lives

The block-pad parameters and arithmetic must live in the lowering layer
(`AmdgpuKernelLowering` `fragCoord` for reads; the staging-store address path for
writes; `CoopStage` for the wide-transposed staging primitive), exposed through a tile
attribute rather than open-coded in the kernel.

### Use cases
- **3.1** As the kernel author, when I declare the B (and A) shared tile with a
  block-pad attribute `(LBSPP, LdsPad)`, then both the staging-write and the WMMA-read
  address computations pick it up automatically with no per-call stride argument.
- **3.2** As the compiler maintainer, when block padding is off (attribute absent),
  then address computation is byte-identical to today's behavior (no regression to the
  shipped per-row-padded kernel or any other kernel).

## 4. PGR2 two-stage global prefetch

A genuine two-stage software pipeline over two LDS buffers: prologue prefetches the
first two K-panels; steady state computes panel N-1 while the global loads for panel N
are in flight; epilogue drains. (The shipped kernel uses naive single-step
double-buffering.)

### Use cases
- **4.1** As the kernel, when the K-loop runs under PGR2, then at steady state the
  global-load instructions for panel N are issued before the WMMA of panel N-1, and the
  result is numerically identical to the non-pipelined kernel (correctness oracle).
- **4.2** As a developer, when PGR2 is enabled, then the kernel still fits registers
  without spills (`.vgpr_spill_count == 0`) — the two-buffer staging must not blow the
  VGPR budget that the device-IR-opt fix freed.

## 5. LRVW16 wide LDS reads

Issue 256-bit-equivalent LDS reads (two `ds_read_b128`) per fragment operand load where
the fragment geometry allows, matching torch's LRVW16.

### Use cases
- **5.1** As the compiler, when a cooperative-matrix fragment load can fetch 16 f16 per
  lane, then it emits two `ds_read_b128` (or the widest available) rather than one,
  into the padded layout (ISA probe).
- **5.2** As the developer, when LRVW16 lands, then on-device throughput is measured vs
  the b128-only read; if it is marginal on gfx11 (ds_read caps at b128), that is
  recorded and the lever kept only if it measures a win.

## 6. Measurement & acceptance tiers

### Use cases
- **6.1** As the developer, when I land each unit, then a GPU-free ISA probe gates it
  (widths preserved, no spill, no `Cannot select`) before any device run.
- **6.2** As the developer, when block padding is on-device, then rocprof reports the
  bank-conflict % for the full kernel, attributing the write-side reduction.
- **6.3** As the developer, when the recipe is assembled, then f16 matmul throughput is
  measured at n2048 vs 26.2 (baseline) and 38.2 (torch), reported against tiers:
  **floor** > 26.2 (beat the shipped kernel), **good** ~32 (≈84% torch), **parity**
  ~38 (≈100% torch). Ship the best measured configuration; record the rest.
