# Kernel drivers: one operation source, a data-movement layer per device

Status: active. Spec and plan approved by Julian 2026-11-22. Opened from gpu-kernel-parity §13 (Julian: "what about having a CPU
specific scheduler and a GPU scheduler? The device specific scheduler would then treat the data as
it needed"; "That could be the solution. Let's give it a try.").

## 1. Definition

**1.1 Purpose.** A hot kernel in cajeta-llm is written twice today, once shaped for GPUs and once
for the cpu. The two agree on the arithmetic and differ in how data reaches the multiplier. This
work splits a kernel into the part that is the same on every device and the part that moves data,
and gives the second part one implementation per device class, written once in the library.

**1.2 The problem, measured (2026-11-22, proton, cajeta 20b996dab).** Running the GPU-shaped
kernels on the cpu: Llama 3.1 8B prefill 47 tok/s against 306 for the cpu-only kernels, decode 12.2
against 22.5; Gemma 3 4B 70 against 650 and 19.5 against 35.9. The prefill GEMM is 7x behind. Its
16x16 int8 tile multiply takes 63 ns on the cpu; a whole tile step takes about 180 ns; the cpu row
form does the same arithmetic in about 24 ns. The difference is staging, tile loads and the
epilogue, which the source asks for and the cpu never needs.

**1.3 What exists.**
- `QkTile<TM, TN, WM, WN, KS>` and nine siblings (Q2, Q3, Q5, Q6, Q8 and the Id forms): one class
  template per quantization, each a full copy of the tile kernel with its own decode. A workgroup
  stages a half block of weights and activations into shared memory behind a barrier, loads
  16x16 fragments, multiplies, and scales in an epilogue. The weight is read in file layout,
  144 bytes a block.
- `q4kQ8X8RowsKernel` and its siblings: the cpu form. One work-item owns eight weight rows on an
  8-row interleaved repack (1216 bytes a block group, written at load) and four activation rows,
  with VNNI dots and sums held in registers. cajeta-llm has 52 cpu-only kernels of this kind.
- The tuner already picks among family members per device (gpu-kernel-parity §11, §12).

**1.4 Terms.**
- **Operation**: what is computed, for example "quantized weight matrix times a batch of
  activation rows".
- **Piece**: device-neutral code the kernel author writes: decode one block, combine a span of
  weights with a span of activations, scale and store a result.
- **Driver**: the per-device code that chooses the blocking, the weight packing, the staging and
  the work-item layout, and calls the pieces.

**1.5 Constraints.**
- A driver and its pieces compile into one kernel per device. Nothing is called across a run-time
  boundary per block or per tile.
- The GPU driver reproduces today's tile kernels: same staging, same barriers, same register
  budget. Tiles remain the GPU's execution form and `CooperativeMatrix` stays in the language.
- Results keep each device's current value contract (7.1; the conformance corpus and llm's value tests).
- cajeta-llm's speed against same-day llama.cpp does not drop on any device.

**1.6 Non-goals.**
- Removing tiles from the GPU, or the software tile from the cpu backend.
- A compiler that derives one shape from the other.
- The attention kernels and element-wise kernels, until the GEMM prototype has passed (§5).

## 2. The pieces

- **2.1** When an author writes a quantized GEMM, the source states the block decode, the combine
  and the scale-and-store once, with no tile, barrier, shared memory or work-item index in it.
- **2.2** When a piece is used by two drivers, both compile the same source. A piece that needs a
  device-specific line is a defect in the split, reported by name.
- **2.3** When the combine is stated, it is over spans ("these weights against these
  activations") so the cpu driver maps it to dot instructions and the GPU driver to a tile
  multiply.
- **2.4** When a new quantization is added, the author writes its decode piece and nothing else;
  every driver then serves it.

## 3. The drivers

- **3.1** When an operation runs on a GPU, the GPU driver cuts it into tiles, stages them, places
  the barriers and runs the epilogue, as `QkTile` does today.
- **3.2** When an operation runs on the cpu, the cpu driver walks rows on its own weight packing
  with sums in registers, as `q4kQ8X8RowsKernel` does today. No tile is built.
- **3.3** When a driver needs the weight in its own packing, the repack happens at load and is
  part of the driver, not the application.
- **3.4** When a device class has more than one good blocking (64x64 and 128x128 tiles, 8- and
  16-row interleaves), the driver exposes them as members and the tuner picks by measurement.
- **3.5** When an application launches the operation, it names the operation only. It holds no
  backend test and no route switch.

## 4. The language and library surface

- **4.1** When the prototype is built, it uses what the language has (class templates with
  `@Instantiate` and `@Requires`, `@Device` helpers, interfaces) before anything is added.
- **4.2** When the language cannot express a driver over a piece (a template parameter that is a
  class whose static `@Device` methods the kernel calls and inlines), that gap is specified and
  closed in the compiler as part of this work.
- **4.3** When a driver is instantiated with pieces that do not fit it, the build refuses by name
  with the reason, as `@Requires` does for tile shapes.

## 5. The prototype and its gate

- **5.1** When the idea is tested, one operation is built first: the Q4_K x int8 prefill GEMM,
  with a GPU driver, a cpu driver and shared pieces.
- **5.2** When the prototype's cpu kernel is measured on proton, it is within 5% of
  `q4kQ8X8RowsKernel` on Llama 3.1 8B and Gemma 3 4B prefill, with the same values.
- **5.3** When the prototype's GPU kernel is measured, it is within 2% of `QkTile` on gfx1151
  (amdgpu and vulkan) and on the RTX 4090 (cuda), with the same values and no more registers.
- **5.4** When 5.2 and 5.3 both hold, the remaining quantizations move to the drivers by adding
  their decode pieces, and their per-quantization kernels and cpu twins are removed.
- **5.5** When either fails, the report says which part of the hand-written kernel the split could
  not carry, and the families route (gpu-kernel-parity 13.4) stays.

## 6. After the prototype

- **6.1** When the GEMM drivers hold, the quantized mat-vecs (decode) get the same split.
- **6.2** When the GEMM drivers hold, attention is examined for a split (score a key, fold it in)
  and either gets a driver pair or stays a family.
- **6.3** When an operation has drivers, gpu-kernel-parity 13.7 counts it as one source.

## 7. Decisions (Julian, 2026-11-22)

- **7.1** Each device's driver matches its current kernel's values: the cpu driver equals
  `q4kQ8X8RowsKernel` bit for bit, the GPU driver equals `QkTile`. One contract for both is not
  part of this work.
- **7.2** The drivers and pieces are built in dev.cajeta.ml.kernels from the start, and
  cajeta-llm consumes them from there.
- **7.3** gpu-kernel-parity Unit 23's attention compiler work and its families work pause until
  the gate of §5 reports.
- **7.4** The GPU tolerance of 5.3 is 2% (approved with the plan).
- **7.5** (2026-11-23) The gate of §5 passes on the cpu and AMD evidence: the remaining
  quantizations move to the drivers. No hand-written kernel is removed until the RTX 4090 row of
  5.3 is in. 5.3's vulkan row is the harness: cajeta-llm routes vulkan prefill to its own
  cooperative-matrix kernels, which the plan's Unit 10 decides separately.
- **7.6** (2026-11-24) The GPU decode mat-vec driver computes every format with the pieces'
  arithmetic. It equals cajeta-llm's Q4_K and Q5_K GPU decode kernels bit for bit. For Q6_K, Q3_K
  and Q2_K, whose hand kernels split a block into 16-byte slices and round in a different order,
  it is held to a small relative tolerance at kernel level and to cajeta-llm's logits KL tests on
  the models. This amends 7.1 for those three GPU decode kernels only.

- **7.7** (2026-11-24) The GPU decode-attention driver equals cajeta-llm's
  `attnFlashDecodeGqa8Kernel` bit for bit. Against `attnFlashDecodeGqa4Kernel` and
  `attnFlashDecodeGqa2Hd256Kernel`, whose source arithmetic is the same but which the compiler
  rounds differently under `@FastMath` (a few ulps), it is held to a relative tolerance at kernel
  level and to cajeta-llm's logits KL on the models.
- **7.8** (2026-11-24) The GPU MoE Id GEMM driver equals cajeta-llm's `Q5IdTile` and `Q6IdTile`
  bit for bit. For Q4_K, whose hand kernel `IdTile` folds the scales in float where the driver
  folds them in integers, the driver's member is held to a relative tolerance at kernel level and
  to cajeta-llm's logits KL on a MoE model before cajeta-llm routes it; `IdTile` stays until the
  RTX 4090 rows confirm the member.
- **7.9** (2026-11-25) The float-activation decode drivers equal cajeta-llm's float decode
  kernels bit for bit (`q80F32WaveMatVecKernel`, `q40F32WaveMatVecKernel`,
  `q50F32WaveMatVecKernel`, `q6kF32WaveMatVecKernel`, and the f16 column forms): each reproduces
  that kernel's summation order, so 7.1 holds and no KL run is needed before cajeta-llm routes
  them.
- **7.10** (2026-11-26) A GPU entry's member choice is stable across runs: a timed choice is
  remembered in the consumer's tune store (cajeta-llm's tune directory) and reused, so a model's
  output does not change between runs of the same build. Where the entry's remembered choice
  differs from the engine's, the entry is held to the engine by logits KL (spec 7.6's measure),
  no further than the engine's own alternative member.
