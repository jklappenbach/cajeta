---
id: xpu-kernel-performance
applies-to: [cajeta/gpu/xpu]
title: Choosing a kernel decomposition — register budget, lane mapping, load width
description: How to pick the decomposition, warp tile, lane-to-data mapping and reduction for an @Kernel, keyed to word size, data volume and arithmetic intensity; and how to measure whether you were right.
---

# Constructing an `@Kernel` that runs at the machine's speed

Every rule below was **measured**, and each names the measurement. Numbers are
from gfx1151 (RDNA3.5, wave32, RADV) unless stated; the *shapes* of the rules
carry across hardware, the constants do not.

The single most important habit: **when two or three source-level changes in a
row measure flat, stop changing the source and ask the shader compiler what it
built.** Wall-clock cannot see register spilling, and spilling is the failure
mode that most often makes a hand-tuned kernel slow.

```
RADV_DEBUG=shaderstats <binary>   # VGPRs, Spilled VGPRs, Scratch, LDS, Subgroups/SIMD
RADV_DEBUG=asm         <binary>   # then count scratch_store/scratch_load in the LOOP body
RADV_DEBUG=nocache,shaderstats    # the pipeline cache suppresses already-compiled shaders
```

---

## 1. Pick the decomposition from the reuse, not from the loop nest

| Problem | Reuse per loaded byte | Decomposition | Bound by |
|---|---|---|---|
| matrix × matrix (GEMM, batch ≥ ~64) | high — each weight byte feeds N outputs | cooperative-matrix tiles, LDS staging | mma issue + register file |
| matrix × vector (batch 1, decode) | **none** — every weight byte is read once | one wave per output row | memory bandwidth |
| elementwise / streaming | none | one item per element, widest vector load | memory bandwidth |

The two matrix cases want *opposite* things, which is why one kernel shape does
not serve both:

- A **GEMM** wants a big warp tile so each staged fragment feeds many mma. Its
  enemy is the register file.
- A **mat-vec** wants as many resident waves as possible so the memory system
  always has requests outstanding. Its enemy is anything that reduces wave
  count — including "efficiency" wins that give each wave more work.

**Measured (mat-vec):** giving each workgroup 2 or 4 output rows instead of 1 —
which halves activation re-reads and quadruples in-flight weight loads, and is
what llama.cpp's Vulkan back end does — measured **worse at every shape but
one**: 14336×4096 went 188 → 160 GiB/s at 2 rows and 154 at 4. Fewer workgroups
meant fewer waves, and a bandwidth-bound kernel would rather have the waves.
Do not "optimise" a mat-vec by giving each wave more rows.

---

## 2. Register budget: compute it before you write the kernel

A `CooperativeMatrix<float32,16,16,2>` accumulator is 256 elements over a
wave32 subgroup = **8 VGPRs per lane**. An f16 A/B fragment is 4. So:

```
warp tile R x C  ->  R*C accumulators * 8   +   (R + C) fragments * 4   VGPRs
```

A 4×4 tile is `16*8 + 8*4 = 160` VGPRs of matrix state alone, before staging
temporaries and addressing. The ceiling is 256. It does not fit.

**Measured:** the 4×4 tile compiled to `VGPRs 256, Spilled VGPRs 74, Scratch
9472`, and the k-loop body carried **68 `scratch_store` + 76 `scratch_load`
against its 64 `wmma`** — accumulator state evicted to VRAM and reloaded every
iteration. Halving to eight accumulators (4×2 or 2×4) gave `VGPRs 192, Spilled
0, Scratch 0`, occupancy 5 → 8 subgroups/SIMD, and **55–70% more throughput**
despite *halving the arithmetic intensity on paper* (8 mma per 6 fragment loads
instead of 16 per 8).

**Rule:** `Spilled VGPRs 0` and `Scratch size 0` are acceptance criteria, not
aspirations. Both are readable from a build with no GPU time and no idle
window, so check them on every new tile shape before you ever time it.

Corollary: arithmetic-intensity arguments are only valid *inside* the
no-spill region. A tile that spills loses more to memory traffic than any
reuse argument can win back.

---

## 3. Word size: make the device layout dword-addressable

A `KernelBuffer<int8>.wordView()` can only address a block whose **stride is a
multiple of 4**. Formats whose natural stride is not get repacked once, on
device, at load time — never per launch.

| natural stride | device stride | why |
|---|---|---|
| 84, 144, 176 B | unchanged | already `% 4 == 0`, read in place |
| 110, 210 B | 112, 212 | stride-only pad; fields already land on dword boundaries |
| 18, 22, 34 B | 20, 24, 36 | a 2-byte scale then a payload at byte 2 — move the payload to byte 4 |

The invariant is worth asserting in a test: **a format needs a repack exactly
when its natural stride is not a multiple of 4**, and the device stride is
never more than 3 bytes larger. A wrong entry silently shifts every block of a
tensor, which is invisible until the outputs are garbage.

---

## 4. Lane → data mapping: the packing dictates it

This is where mat-vec kernels are won and lost, and intuition is a poor guide.
The rule: **a lane must own the elements that share a byte.** Any other mapping
makes several lanes fetch the same byte for different pieces of it.

Worked example (Q6_K, the format that was 39% behind before this was fixed):

- a `ql` byte holds two nibbles, for elements **64 apart**
- a `qh` byte holds four crumbs, for elements **32 apart**

Giving each lane one *contiguous* 16-element sub-block therefore uses half of
each `ql` byte and a quarter of each `qh` byte: 16 lanes requested **512 bytes
per half-superblock for 192 unique ones — 2.67×**. Measured at **136 GiB/s**
where the same engine's Q4_K read 180.

Giving each lane the elements `{16a+j, 32+16a+j, 64+16a+j, 96+16a+j}` — the
stride that makes one `qh` byte's four crumbs and one `ql` byte's two nibbles
all belong to the same lane — requests **48 bytes for 48 unique**, covers 4×
the elements per lane, and measured **190 GiB/s** (llama.cpp's own Q6_K shader,
which uses exactly this structure at 4-element granularity, gets 191).

**How to check your mapping without running anything:** count the bytes the
lanes of one wave *request* per unit of work and divide by the bytes that are
*unique*. Anything above 1.0 is a mapping you can improve; above 2.0 is
probably your bottleneck.

---

## 5. Reductions: subgroup, not shared memory

A workgroup that is exactly one wave should reduce with `Wave.reduceSumF32`,
not an LDS tree. The tree moves through memory what the hardware does across
lanes, and it costs *five barriers* — cheap when the k-loop is long, and the
k-loop is often not: a mat-vec over K=4096 with 8 blocks in flight runs **two**
iterations and then pays the whole tail.

**Measured**, replacing a 5-barrier LDS tree with one `Wave.reduceSumF32`,
every shape improved: 14336×4096 180.9 → 193.8 GiB/s, 4096×14336 171.6 → 177.2,
128256×4096 158.7 → 161.0.

A caveat that is itself a lesson: the same substitution was tried in an earlier
arc as a hand-written shuffle butterfly and measured *exactly neutral* (89.1 vs
88.5 GiB/s) — because the kernel was then running at half this rate, where the
tail was half the share it is now. **A refutation is valid at the baseline it
was measured at.** Re-test cheap changes after the baseline moves.

---

## 6. What did NOT matter (measured, so you needn't retry)

- **LDS store width.** Replacing 32 scalar 2-byte `sa[i] =` stores with four
  16-byte vector stores measured **exactly zero** (8.54 vs 8.54 TMAC/s). The
  driver already merges them, and 32 `ds_write`s are noise beside 144 scratch
  ops.
- **Halving A-side global traffic.** A genuine duplicate read (two k-halves
  loading the same quant dword-run) removed: **2.8–5% slower**.
- **Dequant ALU economy.** One-shift nibble extraction and vectorised copies:
  7.70 → 7.76 TMAC/s, inside noise — because dequant arithmetic was only 7% of
  the kernel, which a decomposition measurement had already said.

The pattern: three consecutive source-level "obvious wins" that measured flat
or negative, all while the real cause sat in the shader statistics. See rule 0.

---

## 7. Language traps that bite inside `@Kernel` bodies

| Construct | What happens |
|---|---|
| `cond ? a : b` | **skipped on one backend, MISCOMPILED on another.** Always `if`-form. This is a hard rule, not a preference. |
| `0u` / unsigned literal suffix | parse error |
| `int32Literal * uint32Var` as an LDS index | crashed the compiler inside `emitSpirv` (SIGSEGV). Build offsets by chaining uint32 adds: `uint32 o1 = o0 + span;` |
| a name used in the body but declared under another spelling | `unsupported construct — assignment to unbound local`, and the kernel is **skipped, not failed** |

**A skipped kernel does not fail the build.** The launch finds nothing
registered and every output buffer reads back zeros — so two zeros compare
equal and a device suite passes having run no device code. Watch the
`kernels skipped: N` count on every build, and make every device test assert
its output is **non-zero** before it asserts the output is right.

---

## 8. Measuring it

- **Cold data.** A repeat loop over one tensor measures the 32 MB last-level
  cache, not DRAM — it once read 338 GiB/s, twice the machine's roofline. Size
  the pool past the cache and cycle it.
- **Match the real dispatch.** Timing a syncing launch when the engine issues
  them back-to-back reported **52 GiB/s** for a shape that actually runs at
  129. Time a `NoSync` batch with one sync at the end; keep a sync-per-launch
  arm only to measure dispatch cost.
- **Interleave arms and take the min** of 5–7 rounds. Single-shot arm timings
  drift ~10% run to run here, enough to read a 5% win as a loss.
- **Never delete work to measure it.** Removing the stores from a staging loop
  measures dead-store elimination — every arm collapses to the empty-loop
  floor. Keep the instruction mix fixed and vary only the *value* written or
  the *address* read.
- **Idle-gate.** Contention on this box has read as a 50% regression more than
  once.

---

## 9. Numerics: reordering a sum changes tokens

Any change to the order of a floating-point reduction — a different lane
mapping, a different reduction, a different tile — changes the last bits. In
greedy decoding that flips whichever position happens to be a near-tie.

Do not treat that as a regression on its own, and do not wave it through
either. **Measure the top-2 logit gap at the divergence.** In the Q6_K remap
above, the two runs agreed on the first 11 tokens and diverged at the one
position whose top-2 gap was `0.0004` — an order of magnitude smaller than
every other position in either run (0.01–0.27). That is a coin-flip, not a
defect. A divergence at a position with a *healthy* gap is a bug.

Keep a host-reference test that holds the kernel to a tolerance the
quantization explains, and keep it separate from token-identity gates.
