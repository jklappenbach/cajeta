# Hardware-profile-driven kernel optimization — research findings

Evidence for the spec and plan that are the deliverable of
`agents/hardware-profile-tuning-research-plan.md`. Opened **2026-09-02**, the
day cajeta-llm's AMD arc closed at unit 61 and Julian's direction block named
NVIDIA next.

**The subject is the general mechanism, not the port.** The question this
research answers is: *what must a hardware profile contain, and what logic can
read it, so that a kernel is optimized for whatever device it lands on?*
NVIDIA/Ada is the **proving** target — the second data point that shows the
logic is general rather than gfx1151-shaped — and gfx1151 is the **regression
oracle**, because the hand-tuned arc already established what good looks like
there (decode 11.79 ms/tok, prefill 0.79). General logic that cannot reproduce
the oracle is not general; it is untested.

**How to read this.** Every row is marked with what backs it:

| mark | meaning |
|---|---|
| **MEASURED** | run on hardware or read out of the built artifact, with the command recorded |
| **READ** | established by reading source or a committed record; cited by `file:line` |
| **CLAIMED** | asserted from vendor documentation or prior knowledge, **not yet verified against this toolchain** — a research item, never a premise |
| **OPEN** | not yet established at all |

Nothing in this file may be promoted from CLAIMED to MEASURED without the
command and its output. The AMD arc's own lesson (`CLAUDE.md` §5) is that three
wrong conclusions in one day came from arguing instead of testing.

---

## 0. The situation, in one paragraph

The AMD arc (cajeta-llm units 45–61, 51 commits pulled 2026-09-02) took decode
from 16.5 to **11.79 ms/tok** and prefill to **0.79 ms/tok**, both past their
gates, on `proton` — a Strix Halo box, gfx1151, RDNA3.5, wave32, ROCm. The
NVIDIA target is this box, **Phoenix**: RTX 4090, Ada, `sm_89`, driver 610.62,
CUDA 13.3 (**MEASURED**, `nvidia-smi`). There is **no ROCm on Phoenix** and no
NVIDIA GPU on proton, so no single machine can run both sides of an A/B. Every
comparison in this work is between two machines or between two builds on one
machine — never both at once.

---

## 1. The instruments

### 1.1 The profiler's device tier on NVIDIA — present, and it is CUPTI

**READ.** `runtime/native/cajeta_rt_prof_cupti.c` (788 lines) is the CUPTI
backend; `cajeta-profiler` plan Unit 12 (NVIDIA backend) is `[x]` on all three
of 12.1/12.2/12.3. It uses `CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL` and
explicitly refuses `..._KIND_KERNEL`, which serializes execution
(`cajeta_rt_prof_cupti.c:302-326`, spec §5.4.1).

**READ, from the committed acceptance record** (`agents/cajeta-profiler-plan.md`
Unit 12.3): both device legs green in run `33349155217`; PHOENIX found CUPTI at
`...\CUDA\v12.9\extras\CUPTI\lib64\cupti64_2025.2.1.dll`, **11/11 entries
bound**, `has_timestamp_callback=1`, `on_wsl=0`. On WSL the timestamp callback
is *refused* — the same box, a different answer, and that difference is
recorded rather than averaged away.

Per-launch overhead on Ada, from audit run `33324968136` (RTX 4090, cc 8.9):

| | ns/launch | delta | resolvable? |
|---|---|---|---|
| untraced | 8077.5 | — | spread 1320 |
| CUPTI-traced | 9035.0 | +957.5 | **NO** — below the noise floor |
| event-bracketed | 22039.5 | +13962 | yes, ~10x noise |

So CUPTI Activity costs less than the run-to-run spread while event bracketing
costs a resolvable ~14 µs/launch. **The caveat is in the record too:** those
numbers measure the *audit probe's* CUPTI usage, not the shipped backend, which
adds an external-correlation push/pop per launch. Re-measuring against the
backend is Unit 12.3.b's own open item and is inherited by this work.

**READ, current source — the backend is complete and wired, not a loader
stub.** `cajeta_rt_prof_cupti.c` carries the activity-buffer machinery
(`cuptiActivityGetNextRecord` consumption, `cuptiActivityFlushAll`) and
external correlation (`cuptiActivityPush/PopExternalCorrelationId`), and
`cajeta_rt_prof_gpu.c` wires them into the capture layer: a CUDA init at
`:531`, a correlation push at `:536` and pop at `:544` around every launch, a
flush at `:550`. The file's own header comment — *"what deliberately does NOT
live here yet: the Activity buffer machinery, external correlation…"* — is
**stale**, written when the loader landed first and never updated as
`381c4863` (12.2.d) closed the rest.

Test coverage is ~19 dedicated cases (`ProfilerCupti`), and they pin the
failure modes rather than the happy path: the serializing kind is never
allowed, a zero or inverted kernel timestamp is rejected and counted, a second
subscriber degrades instead of aborting, a CUDA launch without CUPTI falls back
to the host lane, every launch pushes and pops its correlation id, and *"an
overhead smaller than its spread is not a measurement."*

**Consequence — and the premise this corrects.** NVIDIA profiler support does
not need to be built; it exists, it is wired, and it is tested. What has never
happened is pointing it at cajeta-llm. Whether a 48-layer decode step's ~9 launches/layer
survive the capture ring, and what `gpu_dropped_per_mille` reads on a real
decode, is **OPEN**.

### 1.1b CUPTI on THIS box: 12.0 refuses politely, 13.3 crashes

**MEASURED 2026-09-02**, and it corrects two earlier claims in this file.

**Correction 1: CUPTI was never missing.** `ldconfig -p` shows
`libcupti.so → /lib/x86_64-linux-gnu/libcupti.so.12`, shipped by Ubuntu's
`nvidia-cuda-toolkit` 12.0 package and on the default loader path — so the
loader's **fourth** fallback (the bare soname) has been finding it all along.
An earlier note here said CUPTI was absent; that came from reading a
filesystem-wide `find` before it had finished, and is withdrawn.

**What actually blocks the device tier is WSL2, not absence.** Running
`ProfilerCupti*` on this box: **19 tests, 16 passed, 3 skipped**, and the skip
reason is explicit:

```
CUPTI not armed here: state=2 kinds_enabled=0 degraded=0 ts_status=39 tracing=0
reason(): CUPTI bound but the timestamp callback was REFUSED (CUptiResult 39);
records arrive in CUPTI's own clock domain and §6.9 conversion applies.
Known on WSL2.
```

`state=2` is *bound*. The runtime knows this case, names it, and converts. This
matches the CI record exactly: PHOENIX-native found CUPTI and got the full
path; the WSL leg had the callback refused.

**Correction 2: upgrading CUPTI is contraindicated here — it crashes.**
CUPTI **13.3.75** was installed to `~/.local/cuda-13.3` (user-local; no root
available) and pointed at through `CAJETA_CUPTI_LIB`. Two measurements, cleanly
separated:

| operation | CUPTI 12.0 (installed) | CUPTI 13.3.75 |
|---|---|---|
| dlopen + bind entry points | ✅ 11/11 | ✅ 11/11, `has_timestamp_callback=1` |
| `cuptiActivityRegisterTimestampCallback` | ⚠️ refused, `CUptiResult 39`, handled | 💥 **SIGSEGV, core dumped** |

The crash stack goes through
`/usr/lib/wsl/drivers/…/libcuda.so.1.1` into
`libcupti.so(cuptiActivityRegisterTimestampCallback+0x2d)`. So the older CUPTI
degrades and the newer one takes the process with it.

**Do not conclude "CUPTI 13.3 is broken."** The process also holds CUDA runtime
**12.0** (`libcudart12` is the installed runtime), so this may be
version-mixing rather than a WSL fault or a 13.3 fault. Distinguishing them
needs the full 13.3 toolkit installed system-wide, which needs root. What is
established is narrower and sufficient for now: **on this machine, as
configured, the working configuration is the distro CUPTI 12.0, and
`CAJETA_CUPTI_LIB` must NOT point at 13.3.**

**Defect candidate.** The runtime treats a refusal as a state and degrades
(good), but has no guard against a CUPTI whose major version disagrees with the
in-process CUDA runtime — and the failure mode there is a segfault inside a
`dlopen`'d vendor library, which no amount of downstream honesty can recover.
A version-compatibility check before `RegisterTimestampCallback` would turn a
core dump into one more named state. Worth its own defect spec.

**Consequence for the tuning work.** The device tier on this box is the
**degraded** one (CUPTI's own clock domain, §6.9 conversion) — not broken, but
not the same instrument the AMD arc used. Whether a decode profiled through it
is trustworthy enough to tune against is Unit 0's question, and the native
Windows leg is the fallback if it is not.

### 1.2 The profiling guide does not mention NVIDIA at all

**MEASURED.** `grep -ci 'nvidia\|nvptx\|cuda\|cupti' docs/guide/23-profiling.md`
→ **0**. The guide's "GPU work" section names AMD/rocprofiler and
Vulkan/timestamp-queries, then says *"When neither mechanism is available the
span is the host's submit-to-complete window"* — which reads, to anyone
profiling on NVIDIA, as *there is no device tier for you*. Unit 12 shipped one
and the guide never learned. No other file under `docs/` mentions CUPTI
(`grep -rl CUPTI docs/` → empty).

This is the single most expensive doc gap for this work: the user asked for the
profiler to be the instrument, and the document that teaches it currently
tells an NVIDIA reader the opposite of what shipped.

### 1.3 `KernelIsa` is AMD-only, by construction

**READ.** `cajeta-llm/src/main/cajeta/dev/cajeta/llm/bench/KernelIsa.cajeta:12-31`.
It carves ELF code objects out of the built exe — *"an ELF whose `e_machine` is
`EM_AMDGPU`"* — writes them to `tmp/hsaco/`, and asks `llvm-readelf` for the
kernel descriptor (VGPRs, SGPRs, spills, scratch, LDS) and `llvm-objdump` for
disassembly, counting memory and wait instructions.

This is the instrument that produced the arc's two most load-bearing findings:
*"Spilled/scratch 0 as an acceptance criterion"*, and a run of single loads each
behind `s_waitcnt vmcnt(0)` — worth **205 → 224 GB/s** on the q6k head. Unit 60
then used it to establish the k-quant kernels were **ALU-bound, not
DRAM-bound**, which wall-clock alone could never have shown.

**There was no NVIDIA half, and the premise for building one is now
MEASURED** (2026-09-02, this box):

| fact | result |
|---|---|
| a `ptxas -arch=sm_89` cubin is an ELF | yes — ELF64, `Machine: NVIDIA CUDA architecture` |
| its `e_machine` | **190 (0xBE)** — so the same structural carve the AMD half does on `EM_AMDGPU` (224) works |
| target arch recoverable | yes, **but not from `e_flags`** — the SM byte MOVED between ptxas 12.0 (`0x00590559`, low=89) and 13.3 (`0x06005904`, low=4). Read it from the disassembler, whose spelling also changed (`EF_CUDA_SM89` → `.target sm_89`). `e_machine`=190 is stable across both |
| per-kernel resources | `cuobjdump -res-usage` → `REG:23 STACK:0 SHARED:0 LOCAL:0 CONSTANT[0]:372` |
| disassembly + instruction counting | `nvdisasm -c` works; `LDG/STG/LDS/STS/BAR` counting verified |
| how cajeta embeds it | `NvptxRegistration.cpp:112-135` — a **private** host constant `xpu.cubin.<entry>` registered by a ctor, which is exactly why the carve must be structural rather than symbol-driven |

The arch-per-object readout has a second use nobody asked for: it makes
`xpu-cache-discriminator`'s silent wrong-backend artifact (§1.4) **visible by
inspection**.

Split out 2026-09-02 into `specs/kernel-artifact-inspection-spec.md` +
`agents/kernel-artifact-inspection-plan.md`. A commit in the pulled range (`cfdf243`) also records that
KernelIsa's own `tmp/hsaco` staleness read as a current build *for an hour* —
whatever is built for NVIDIA inherits that lesson: clear the staging directory
first.

### 1.4 The cache discriminator ignores the backend — A/B measurement hazard

**READ**, `specs/INDEX.md` (`xpu-cache-discriminator`, filed 2026-09-02, draft):
`--xpu-backend` and `--xpu-arch` do not feed the incremental cache
discriminator. `--print-cache-discriminator` returns the **same**
`63ed014a…89d0` for cpu, amdgpu/gfx1151, nvptx, sm_89, and no xpu flags at all
— five materially different builds, one key. End to end on
`samples/kernel-profile`, a "gpu" binary reports `active backend: cpu`, and the
artifact **shas differ**, so a hash comparison says the toolchain did its job.

**This work is the exact scenario that defect punishes**: alternating amdgpu and
nvptx builds of the same tree is what tuning *is*. Until it is fixed, every
measurement needs a purged cache, and `samples/kernel-profile/run.sh`'s
purge-every-build workaround is the pattern to copy.

---

## 2. Compiler backend asymmetry — nvptx vs amdgpu

**READ**, by extracting the `override` sets from
`src/cajeta/xpu/nvidia/NvptxKernelLowering.cpp` and
`src/cajeta/xpu/amd/AmdgpuKernelLowering.cpp`. Both subclass the same
`LoweringTarget` vtable (`src/cajeta/xpu/lowering/LoweringTarget.h`), so what a
backend does *not* override is what it silently inherits.

| `LoweringTarget` hook | amdgpu | nvptx | base behaviour when not overridden | NVIDIA analogue |
|---|---|---|---|---|
| `schedBarrier` / `schedGroupBarrier` / `schedPriority` / `schedPipelineOpt` | ✅ | ❌ | **no-op** (`KernelLowering.cpp:6099-6106`) | **CLAIMED: none.** PTX has no `sched_group_barrier`; ptxas schedules. Needs a decision, not an implementation |
| `swizzleAddr` | ✅ | ❌ | **identity** (`KernelLowering.cpp:6110`) | **CLAIMED: applies.** 32 banks x 4 B on NVIDIA too; XOR swizzle is the same trick |
| `blockPadAddr` | ✅ | ❌ | **identity** (`KernelLowering.cpp:6117`) | **CLAIMED: applies** |
| `integerDot4x8` | ✅ (`archHasDot4` → `v_dot4`) | ❌ | portable **widening reduce** (`KernelLowering.cpp:2043-2052`) | **CLAIMED: `dp4a`, sm_61+**, `llvm.nvvm.idp4a.*`. Directly on the k-quant path |
| `integerDotWide` | ❌ | ❌ | `nullptr` → per-lane fallback (correct on GPUs by design) | n/a — CPU-shaped hook |
| `coopMatrixEpilogueSupported` | ✅ | ❌ | unsupported | **OPEN** |
| `coopMatrixFromWordsSupported` | ✅ | ❌ | unsupported | **OPEN** — `CooperativeMatrix.fromWords` (`7801a02f`, llm spec 15.21.4) is **AMD-only today** |
| `waveRotate`, `waveShuffleDivergent`, `waveReduceF32` | ✅ | ❌ | portable fallback | **CLAIMED**: `shfl.sync` variants; `redux.sync` on sm_80+ |
| `devicePrintf` | ❌ (deferred: needs hostcall) | ✅ | rejected | already there |
| `readClock` | ❌ | ✅ | default | already there |
| texture family (`fetchTexture{,1D,3D,2DArray}`, `sampleTexture{1D,3D,2DArray,Cube}`) | ✅ | ❌ (2D + image only) | unsupported | not on the LLM path; gfx only |
| `applyOccupancy` | ✅ | ✅ (`maxntid`/`minctasm`/`maxRegisters`) | — | — |
| `asyncCopy` / `asyncCommit` / `asyncWait` | ✅ | ✅ (`cp.async`) | — | — |
| `waveWidth`, `waveLaneId`, `waveShuffle`, `waveBallot`, `waveReduce`, `workgroupBarrier`, `memoryFence`, `threadId`/`workgroupId`/`workgroupDim`/`gridSize`, `prepareNativeCoopMatrix`, `coopMatrix{Type,Load,Store,Splat,MulAdd,Tier}` | ✅ | ✅ | — | at parity |

**The shape of the gap:** NVPTX is not a stub — it has coop-matrix, async copy,
occupancy control, the wave basics, and it is the **default** `--xpu-arch`
(`src/main.cpp:203,1059` — `sm_89`, with amdgpu defaulting to gfx1151 only when
`--xpu-arch` is not pinned). What it lacks is precisely the set of hooks the
AMD arc leaned on for its last 4 ms/tok.

**The silent-degradation problem.** Three of those gaps degrade with **no
diagnostic**: a `Schedule.groupBarrier(...)` compiles to nothing, and a swizzled
or block-padded `Shared` tile addresses linearly. The base implementations say
so outright — *"a scheduling hint is an optimization directive — omitting it
never changes the kernel's result"*, *"perf-only transform; a backend without it
stays correct by addressing the tile linearly"*. Correct, and correctly
reasoned; but it means a kernel tuned on AMD and rebuilt for NVIDIA loses its
LDS swizzle **silently**, which is exactly the failure mode the AMD arc kept
catching with KernelIsa rather than with wall-clock.

---

## 3. The device model has no NVIDIA in it

**READ.** `src/cajeta/xpu/core/DeviceProfile.cpp:37-43` — `kArchTable` has
**two rows, both gfx** (`gfx1151`, `gfx1100`). `queryLiveDeviceModel()` calls
`cajeta_xpu_query_raw_device`, whose implementation
(`runtime/native/cajeta_xpu_driver.c:797-830`) is **HIP-only**: it initialises
HIP, reads the gfx arch string, and fills every attribute through
`hipDeviceGetAttribute`. There is no CUDA path in the query.

**MEASURED on this box, 2026-09-02** — `./build/src/cajeta gpu-profile` with an
RTX 4090 present and CUDA 13.3 installed:

```json
{"arch":"unknown","cu":0,"wave_size":32,"regs_per_mp":196608,
 "max_waves_per_mp":64,"lds_bytes_per_mp":65536,
 "estimated":true,"roofline_measured":false}
```

It does not identify the card **at all** — `arch: "unknown"`, `cu: 0` — and
every occupancy input is the gfx1151 default. **Re-taken on a freshly built toolchain** (HEAD `7dead747`, binary linked
2026-09-02 19:48, after a full rebuild): **byte-identical output.** The first
reading was on a 552-commit-stale binary; this one is not, so the finding is
final.

So on Phoenix the model falls back to `DeviceModel`'s defaults
(`DeviceProfile.h:47-61`), described in the source as *"a conservative
gfx1151-shaped baseline"*:

| field | default | true on RTX 4090 / Ada (**CLAIMED**) | consequence |
|---|---|---|---|
| `waveSize` | 32 | 32 | correct **by coincidence** — RDNA3 and NVIDIA agree |
| `ldsBankCount` / `ldsBankWidth` | 32 / 4 | 32 / 4 | correct by coincidence |
| `regsPerMP` | 196608 | 65536 | **3x over** |
| `maxWavesPerMP` | 64 | 48 | over |
| `ldsBytesPerMP` | 65536 | 102400 (99 KB usable) | under |
| `cuPerMultiprocessor` | 2 (RDNA WGP = 2 CU) | 1 | **CU count doubled** |
| `estimated` | `true` | — | honestly flagged, and nothing reads the flag as a refusal |

The flag is the redeeming part: `estimated` stays `true`, so the model does not
*claim* to be measured. But every occupancy-derived decision on NVIDIA is
currently taken against a gfx1151 machine model with a doubled CU count, and
`xpu-kernel-scheduling`'s offline classification (INDEX: *"reuses the shipped
`DeviceProfile` roofline + occupancy budgets"*) is built on it.

---

## 4. The engine's portability surface

**MEASURED** over `cajeta-llm/src/main`:

| probe | count |
|---|---|
| `Wave.width()` call sites | **0** |
| `Schedule.*` call sites | **0** |
| launches with a literal `block: [256` | 71 |
| … `block: [32` | 63 |
| … `block: [64` | 23 |
| … `block: [128` | 26 |

Against the stdlib's own stated doctrine
(`runtime/src/cajeta/xpu/Wave.cajeta:30-33`): *"write wave-cooperative kernels
in terms of `laneId()` and `width()` — **never a hardcoded width** — and the
same source is correct on NVIDIA (32), AMD (32/64), Vulkan (runtime), and CPU"*.
The engine violates that doctrine in 183 launch sites and honours it in none.

**This is less bad than it sounds, and the reason matters.** gfx1151 is
**wave32**. `Linear.cajeta:156` says so in as many words: *"5120 rows / 64 rows
per workgroup = 80 workgroups x 2 wave32 waves"*. So the wave geometry the arc
tuned against is the same 32 lanes NVIDIA has, and a `block: [64]` is two waves
on both machines. The hardcoding is a **latent** portability defect (it breaks
on CDNA wave64, and it defeats any width-parameterised retune), not necessarily
a live correctness bug on Ada. Which of the 183 sites actually assume a width,
versus merely picking a block size, is **OPEN** and is the first real audit.

**What is NOT portable, and is stated as such in the record:** unit 61's whole
premise. The plan's own entry reads *"PREMISE REFUTED: the v_mad_u64 were the
32-bit `Vector<int32,8> * scalar` scale multiplies …, **QUARTER-RATE on
RDNA3**"* — an RDNA3 issue-rate fact. The fix (reduce first, scale the scalar)
is integer-exact and harmless anywhere, but its *payoff* is RDNA3-shaped, and
several other arc findings are the same shape: word-form nibble assembly (u60),
the saddr addressing form (u61), the 238 GB/s ceiling the q6k head was measured
against (u58). **On Ada each of these is an open question, not a carried
result.**

---

## 5. Module responsibility

### 5.1 cajeta-ml is not on the device path at all

**MEASURED.** Of 144 `.cajeta` files under `cajeta-ml/src/main`: **0** contain
`@Kernel`, and **1** imports `cajeta.xpu` (`ml/grad/Ops.cajeta`). That one file
is where the boundary is declared: `hasGpu()` answers false and `requireCpu`
throws on a device-resident tensor (`ml/grad/Ops.cajeta:13,32,36`) — recorded
in the llm plan's 2026-08-08 audit as *"the `dev.cajeta.ml` nn stack cannot
participate in inference; only its `state_dict` naming and checkpoint
reconciliation are reused."*

So **cajeta-ml is out of scope for tuning** and in scope only for the
responsibility review. The structural question it raises is real: two libraries
own tensor math, one CPU-only with autograd, one device-only without, and
nothing yet says which owns what.

### 5.2 Candidates in cajeta-llm that may belong elsewhere

**READ** — each is a smell to be adjudicated, not a verdict:

| unit | what it is | the question |
|---|---|---|
| `tok/StrIntMap` (102 lines) | hand-rolled open-addressing `String`→`int32`, linear probing, grows at 70% | the stdlib has `HashMap`, and `agents/swisstable-hashmap-plan.md` exists. Why was this hand-rolled — measured need, or absence? |
| `tok/ByteBuf` (27), `tok/IdBuf` (37) | growable `int8[]` / `int32[]` | stdlib buffer/builder surface |
| `model/HostOps` (147) | *"straightforward f32 loops over flat offsets… EVERY method here is allocation-free"* | `cajeta.math` — or deliberately duplicated because allocation-freedom is the load-bearing property? |
| `bench/Metrics` (117) | cosine similarity, top-1 agreement, KL divergence | `cajeta.math.stats`, or `dev.cajeta.ml` |
| `store/I64Codec` (29) | `Encoder<int64>`, little-endian | `dev.cajeta.codec` |
| `io/QuantKernel` (10,932!), `io/WmmaKernel` (5,610) | k-quant kernels; WMMA GEMM | the GEMM primitives at least are candidates for `cajeta.xpu` / `nucleo` — cf. the `xpu-pipelined-gemm-primitives` draft spec |
| `bench/KernelIsa` (236) | code-object reader | a **toolchain** instrument living in an application repo |

`io/QuantKernel.cajeta` at **10,932 lines** is on its own the largest single
question in the review: it is where the arc's tuning landed, and any line of it
that is really "how you write a fast quantized mat-vec on this compiler" is
stdlib or skill material rather than engine material.

### 5.3 Direction of travel is already documented, and it is AMD-shaped

- `docs/specification/xpu/CajetaXPU-Variance.md` — the register of where the
  backends diverge — is written from the **NVIDIA-first** era: its §4 is
  literally *"Hard pre-AMD checkpoints"*, and its preamble describes keeping
  divergences *"from leaking into `cajeta.xpu` as NVIDIA-shaped assumptions
  during the NVIDIA-first implementation phase."* The world has since inverted.
  Its 12 numbered rows are still the right axes; row 1 (wave width) already
  states the rule the engine breaks.
- `runtime/src/cajeta/xpu/skills/xpu-kernel-performance.md` (219 lines) is the
  skill an agent gets when writing a kernel. **MEASURED**: 11 AMD/gfx/LDS/hsaco
  mentions against 3 NVIDIA/warp. Its §6 *"What did NOT matter (measured, so you
  needn't retry)"* is exactly the kind of section that is **arch-specific and
  reads as universal**.

---

## 6. Validation surface

- **The engine has no device CI.** llm plan 7.3.1: *"every primitive runs its
  DEVICE path on the in-process CPU backend … NVIDIA/AMD/Vulkan runs ride the
  device-tests discipline — **the engine repo has no device CI yet**, and Unit
  2's bf16-fragment abort showed kernel-lowering bugs hide until every backend
  actually compiles the kernels."*
- **The compiler's device CI is entirely NVIDIA.** `.github/workflows/device-tests.yml`
  computes its matrix from two legs: `wsl-nvidia` and `windows-nvidia`
  (self-hosted, this box). Its header records why it exists: on a machine with
  no NVIDIA card the CUDA legs **skip silently and the suite reports green** —
  measured 2026-08-09 on proton, *"a full 129-test math regression passed
  without a single CUDA kernel ever executing."* The gate against that is the
  workflow's own "Fail if nothing actually ran on hardware" step.
- **Parity harness exists and is host-shaped**: `cajeta-llm/tools/parity/`
  (`run-parity.sh`, `run-bos-probe.sh`, fixtures, `gen_fixture.py`), plus
  `bench/ParityRun`, `bench/GpuParity`, `bench/Metrics`, `bench/RunReport`, and
  ~30 `bench/*Probe` classes. The arc's standing result is *"generation
  identical throughout (last token 34208)"* — **an exact-token criterion**,
  which across two vendors is **OPEN**: different reduction orders and
  contraction choices make bit-identity between AMD and NVIDIA an assumption to
  test, not a gate to assume.
- **Dependency checkouts** needed to build/test the engine locally are now all
  present as siblings: `cajeta-unit` (0.2.3), `cajeta-codec`, `cajeta-jinja`,
  `cajeta-logging` (0.7.0), `cajeta-http` — resolved by `run-tests.sh` through
  `cajeta artifact-path`, sibling-checkout first.

---

## 7. The general-purpose lever already exists — and nothing pulls it

This is the most important finding for the reframed question, and it is good
news twice over: the machine model is designed, shipped, and closed; and the
one design that would have made it *general* was dropped for a reason that
later measurement has undercut.

### 7.1 What shipped (archived specs, work complete)

`specs/archive/xpu-device-profile-spec.md` (312 lines) and
`specs/archive/kernel-occupancy-autotune-spec.md` (161) are both closed. Between
them, **READ** from `src/cajeta/xpu/core/DeviceProfile.h:80-140`:

| surface | what it does |
|---|---|
| `DeviceProfile{ model, bandwidthGBps, peakGFLOPs, rooflineMeasured }` | the machine model + a **measured** memory ceiling; `peakGFLOPs` deferred (0 = unknown) |
| `occupancy(model, block, kernelVgpr, ldsBytes)` | closed-form resident waves/MP — min over register, LDS and wave-residency limiters. **Topology-free**, built only from per-MP quantities |
| `candidateBlocks(...)` | feasible wave-multiple block sizes, best-first by predicted occupancy |
| `classifyBound(flops, bytes, bw, peak)` | roofline verdict against the ridge point; `Unknown` when a ceiling is missing |
| `LaunchPick{ block, occupancyWaves, bound, geometryWontHelp, advisoryOnly }` | the picker's verdict, **including the honest negative** — `geometryWontHelp` when memory-bound |
| `cajeta gpu-profile` | interrogates the live device, prints the profile as one-line JSON |
| automatic workgroup-size-aware **register budgeting** | compile-time; tells the backend the true launch size so it stops budgeting for the hardware-maximum workgroup. Measured origin: the AMDGPU allocator capped the f16 WMMA GEMM at 192 VGPR and **spilled 84 registers** |
| `@Occupancy` | the portable expert override; **no-op** where a backend has no equivalent |

That is a genuine analytic tuner: it derives a launch configuration from
hardware facts plus the kernel's compiled resource demand, with no timing
sweep, and it says so when geometry cannot help.

### 7.2 Nothing consumes it

**MEASURED.** The files mentioning `DeviceProfile` / `DeviceModel` /
`queryLiveDeviceModel` anywhere in `src/`:

```
src/main.cpp
src/cajeta/xpu/core/DeviceProfile.{h,cpp}
src/cajeta/cli/XpuProfileCommand.{h,cpp}
```

That is the **CLI and its implementation, and nothing else**. No lowering pass,
no launch path, no runtime, and no stdlib call site reads the picker. The
profile is a thing you can *print*, not a thing that *decides*. The spec is
consistent with this — §4.2 says the picker "does not override the author; it
emits the roofline prediction as advisory diagnostics", and §1.3 makes advisory
status explicit for hand-tuned kernels — but it means the general logic the
reframed question asks for currently terminates at a JSON dump.

### 7.3 The one design that would close the loop was dropped — on a premise the arc has since undercut

`kernel-occupancy-autotune` §4, **DROPPED 2026-06-28**: the three-tier lookup
(runtime cache → **shipped guidance table** keyed by (arch, kernel,
problem-shape) → bounded sweep on a miss). Its stated reason:

> Runtime config search … earned its complexity for no real workload:
> profiling showed the f16 GEMM is **memory-bandwidth-bound** (rocprof
> MemUnitBusy ~86%), so block-size search cannot beat the bus.

`xpu-device-profile` §1.2 then also ruled out "a persistent tuning database or
shipped-guidance table", keeping only a **bounded sweep gated to unmodelable
devices** (§4.5).

**What has changed since**: cajeta-llm unit 60 (`2d1a301`, pulled today) —
*"the k-quant kernels were **ALU-bound, not DRAM-bound**: word-form nibble
assembly and shift-picked scales halve their instruction count; 12.3 → 11.9
ms/tok"*. And unit 61 found the real cost was quarter-rate 32-bit multiplies on
RDNA3, not the memory system at all.

These do **not** contradict the 2026-06-28 measurement — different kernel, and
the f16 GEMM really is bandwidth-bound. What they undercut is the
**generalization** drawn from it. The drop reasoned from one kernel class to
"no real workload"; the decode path is a second class where the premise is
false, and it is the workload that now matters most. Whether that reopens §4 —
and if so, whether as shipped guidance, as a bounded sweep, or as neither — is
**Q5**, and it must be decided from measurement on both machines, not from
either of these two data points alone.

### 7.4 Capability is not in the profile — it is in a C++ vtable

**READ.** Every "can this device do X" fact — dp4a, coop-matrix tiers, LDS
swizzle, scheduling hints, wide texture sampling — is expressed as *whether a
`LoweringTarget` subclass overrides a virtual method* (§2). None of it is data,
none of it reaches `DeviceProfile`, and none of it is queryable from cajeta
source. A profile that is to drive general optimization needs the ISA
capability set as **facts**, alongside the occupancy and bandwidth facts it
already carries — otherwise "optimize for any hardware profile" can only ever
mean "pick a block size".

---

## 8. The arc's constants, and which a profile could derive

The AMD arc's 51 commits are a corpus of tuning decisions with their
measurements attached — which makes it the best available inventory of *what a
general optimizer would have to decide*. Each row below is a decision the arc
took by hand, paired with the hardware fact that determines it and where that
fact stands today. **READ** from the pulled commits and the plan's unit
records.

| arc decision | hardware fact that determines it | in the profile today? |
|---|---|---|
| block sizes 32 / 64 / 128 / 256 across 183 launches | wave width; regs/MP; LDS/MP; the kernel's VGPR + LDS demand | **yes** — `candidateBlocks`/`occupancy` compute exactly this, and no launch calls them |
| "64 rows per workgroup = 80 workgroups x 2 wave32 waves" (`Linear.cajeta:156`) | wave width, CU count | `waveSize` yes; `cuCount` yes (**doubled on NVIDIA**, §3) |
| fuse 9 launches per layer to close **2.2 µs** gaps (units 47–59) | **per-launch overhead** | **no field exists** |
| q6k head tuned to a **238 GB/s** ceiling; achieved 232 (unit 58) | measured DRAM bandwidth | **yes** — `bandwidthGBps`, and the arc measured its own instead |
| "reduce first, scale the scalar" — 32-bit multiplies are **quarter-rate on RDNA3** (unit 61) | per-op **issue rate** by dtype/width | **no field exists** |
| word-form nibble assembly; byte ops in WORD form (unit 60 + compiler `8c9369d3`) | native access granularity; whether sub-dword ALU ops exist | **no field exists** (it is a lowering behaviour) |
| KV planes in **f16** (unit 57, llm spec 15.21.6) | dtype support + bandwidth pressure | partly — a policy question, not just a fact |
| flash loop **four positions deep**; reduce-pack as **eight waves** | register budget; LDS; wave residency | inputs **yes**, no consumer |
| serial-cutover threshold on the CPU backend's worker pool | core count | outside `DeviceProfile` |
| `Spilled/scratch 0` as an acceptance criterion (KernelIsa) | register file size | **yes** (`regsPerMP`) — and the check is a separate AMD-only tool |

Two of those rows are the reframed work's centre of gravity: **per-launch
overhead** and **per-op issue rate**. They are the facts the arc's last 4 ms/tok
turned on, neither is modelled, and both are cheaply **measurable** on any
device — which makes them candidates for the profile rather than for a table of
vendor lore.

A third observation belongs here because it is the sharpest available example
of why hardcoding loses. **CLAIMED, and not yet comparable**: the arc's launch
gap on gfx1151 is **2.2 µs**, while the profiler's Ada audit measured **~8.08
µs/launch untraced** (§1.1). If those numbers are measuring the same thing,
launch fusion is worth *more* on the 4090 than on Strix Halo, and a general
optimizer would fuse **more aggressively** on the machine with the faster GPU —
a conclusion no amount of porting the AMD constants would ever reach. They are
probably **not** measuring the same thing (one is a gap between kernels in a
stream, the other a probe's submit cost), and making them comparable is an
early unit of the plan, not a footnote.

---

## 9. State of the art — what other systems do about this

Surveyed 2026-09-02. The reframed question is not novel, and the literature has
converged on an answer sharp enough to design against. Two results below are
the load-bearing ones: **tritonBLAS**, because it is the strongest evidence
that an analytical model can *replace* search; and **HipKittens**, because it
is the sharpest statement of what does not port.

### 9.1 Analytical selection has caught up with empirical search

**tritonBLAS** (AMD, arXiv 2512.04226, submitted 2025-12-03) is the closest
prior art to what this work proposes, and its central claim is the one that
matters:

> "The model is parameterized **only by measurable hardware rates**
> (bandwidths, instruction latencies, and matrix-core shapes), enabling
> retargeting to new GPU generations via **microbenchmark-based calibration**."

Measured: **94.7% of exhaustive autotuning performance across 150,000 GEMM
shapes** and real-world LLM workloads, with **autotuning time reduced to
zero** and configuration selection at microsecond overhead — which also makes
it viable for dynamic shapes, where a search-based tuner cannot go. Its model
is built on five pillars: hierarchical tiling structure, quantifying
parallelism (spatial loop unroll), quantifying locality, the tradeoff between
parallelism and locality, and schedule latency. It assigns each tile level a
compute / memory / logical **scope** — register, SIMD, wave, shared memory, CU,
workgroup, L2, group-of-CU, XCD, LLC, device — i.e. a per-architecture scope
table, which is a hardware profile in exactly the sense this plan means.

**Why this matters here, concretely.** cajeta's `DeviceProfile` already carries
one of tritonBLAS's three rate families (measured bandwidth) and none of the
other two (instruction latency, matrix-core shape). Findings §8 arrived at
"per-op issue rate is the missing fact" from the *arc's own decisions*, with no
knowledge of this paper. Two independent routes to the same missing field is
the strongest signal in this document.

Neighbours in the same family: **CUTLASS/cuBLAS** use hand-engineered
heuristics over threadblock tiles (memory traffic, register pressure,
tensor-core utilization) — analytical, but authored per vendor rather than
parameterized. **DeLTA** extends the roofline with a locality term, predicting
L1/L2 traffic from cache-blocking and reuse-distance analysis, but analyzes
*predefined* configurations rather than ranking a space.

### 9.2 Empirical search is mature, portable, and expensive

**Kernel Tuner**, **CLTune**, **KTT** and **ATF** are the established generic
autotuners; KTT adds online tuning and *pipeline* tuning across several kernels
at once, and reports results across NVIDIA, AMD, CPU and Xeon Phi — the
performance-portability claim is well supported. **AutoTVM / Ansor** combine
analytical reuse estimates with learned corrections, at the cost of a fitting
phase and weak zero-shot behaviour on new shapes. **Triton**'s own
`@triton.autotune` searches a grid of block sizes × `num_warps` × `num_stages`
on device, which is precisely the cost tritonBLAS removes.

**hipBLASLt / TensileLite** is the shipped-guidance design in production: tuned
kernel libraries plus an offline tuning utility, with **Stream-K** used to get
"consistently good peak GEMM performance with **far fewer tuned kernels**."
Its documented failure mode is worth copying into our own spec verbatim:
TensileLite *"is only as good as the search space you hand it. If the parameter
combinations in your configuration cannot reproduce the best kernel you already
have, that kernel is not even a candidate, and the run can return something
slower than where you started."*

This is directly relevant to **Q5**. The dropped `kernel-occupancy-autotune` §4
was a shipped-guidance-plus-sweep design. The state of the art says: shipped
guidance works and is what gives vendor libraries their edge; a blind sweep is
both expensive and capable of regressing; and an analytical model now reaches
~95% of exhaustive search without either. That ordering — **analytical first,
guidance second, sweep last and bounded** — is a defensible spec position and
matches what `xpu-device-profile` already chose (§4.5, sweep gated to
unmodelable devices).

### 9.3 Abstractions port; schedules do not

**HipKittens** (Stanford Hazy Research, arXiv 2511.08083, 2025-11-09) ported
ThunderKittens' tile abstractions to AMD and reported the boundary precisely:

> "Tile-based abstractions used in prior DSLs **generalize** to AMD GPUs;
> however, the **algorithms that instantiate these abstractions** for AMD need
> to be rethought."

Their worked example is exactly our shape of problem: **wave specialization**,
a standard NVIDIA schedule, underperforms on CDNA3/CDNA4 because AMD's static
register allocation means producer waves consume registers without computing,
capping the output tile per thread block — **80% of peak BF16 GEMM on MI355X**.

**The design consequence for this work is a line, not a warning.** The
*abstraction* (a tile, a wave-cooperative reduction, a launch geometry
expressed in facts) is portable and is what the profile should parameterize.
The *schedule* (wave specialization, four-deep flash loops, eight-wave
reduce-pack) is architecture-contingent, and a general optimizer must be able
to hold several and choose — not to derive one from first principles. This is
the same line Unit 3.2.3 draws as **derivable / searchable / authorial**, and
the literature says the middle category cannot be eliminated, only shrunk.

### 9.4 The reference workload's own answer

**ggml / llama.cpp** — the architecture reference for cajeta-llm — solves this
by *not* solving it: parallel hand-written backends (CUDA, HIP, Vulkan) with
runtime selection from detected features, tensor type, operation shape and
backend priority. Its Vulkan backend does read per-GPU facts (warp size and
shared-memory size: NVIDIA 32 / 49152 B, RDNA 64 / 65536 B) and branch on them.
That is the low bar this work should clear: cajeta already has a single source
compiled to four backends, so the question is only whether the *parameters* can
follow the same path as the code.

### 9.5 Launch overhead is a first-class cost in the literature too

Reported CUDA null-kernel launch overhead is **~3–7 µs**, with the standard
framing that a 10 µs kernel behind a 5 µs launch is a 50% tax, and that ten
such operations can accumulate 300–700 µs — which is why CUDA Graphs exist.
Against the arc's measured **2.2 µs** inter-kernel gap on gfx1151, this makes
the §8 hypothesis more likely, not less: **launch overhead differs enough
between these two machines to invert a fusion decision**, and it is a
first-class field in every serious cost model.

Note the adjacent lever cajeta has not yet reached: graph capture. Variance row
10 lists CUDA graphs / HIP graphs / Vulkan secondary command buffers as
"likely vendor-only, not core" — a judgement made before launch overhead was
known to be worth ~1 ms/tok of the arc's remaining headroom.

### 9.6 Calibration has a literature to borrow from

The instruction-latency and cache-latency numbers this work needs are exactly
what the microbenchmarking papers publish — *Benchmarking and Dissecting the
Nvidia Hopper GPU Architecture* (2402.13499), *Dissecting the NVIDIA Hopper
Architecture* (2501.12084), and the Ampere equivalent — using per-instruction
latency/throughput loops and P-chase for the memory hierarchy, with **RTX 4090
figures among the published comparisons**. Their method is the template for
1.1.2/1.1.3's probes, and their published values are a cross-check on ours:
a calibration probe that disagrees with the literature by 2x is measuring the
wrong thing.

### 9.7 Sources

- tritonBLAS — https://arxiv.org/abs/2512.04226
- HipKittens — https://arxiv.org/abs/2511.08083
- Kernel Tuning Toolkit (KTT) — https://arxiv.org/pdf/1910.08498
- CLTune — https://arxiv.org/pdf/1703.06503
- Benchmarking suite for kernel tuners — https://arxiv.org/pdf/2303.08976
- Autotuning GPU kernels via static and predictive analysis — https://arxiv.org/pdf/1701.08547
- Analytical performance estimation during code generation — https://arxiv.org/pdf/2204.14242
- Dissecting NVIDIA Hopper (microbenchmarking) — https://arxiv.org/pdf/2501.12084 · https://arxiv.org/abs/2402.13499
- hipBLASLt offline tuning + tuning utility — https://rocm.docs.amd.com/projects/hipBLASLt/en/develop/how-to/how-to-use-hipblaslt-offline-tuning.html
- Reverse-engineering hipBLASLt TensileLite kernels — https://rocm.blogs.amd.com/artificial-intelligence/reverse-hipblaslt-tensilelite/README.html
- Triton `Config` (num_warps / num_stages) — https://triton-lang.org/main/python-api/generated/triton.Config.html
- ggml/llama.cpp backend behaviour — https://github.com/ggml-org/llama.cpp/discussions/10879

---

## 10. What is OPEN — the questions the research must answer

Carried into `agents/hardware-profile-tuning-research-plan.md` §Question
register. The reframed set, in short:

**About the mechanism**
- What belongs in a hardware profile beyond what `DeviceProfile` already
  carries — at minimum **per-launch overhead** and **per-op issue rates**, and
  the **ISA capability set** that today exists only as vtable overrides (§7.4).
- Which facts are *queried* (driver attributes), which are *tabled* (arch
  constants), and which are *measured* (probes) — and what a device that
  answers none of the three is allowed to assume.
- Where the derived decision is *applied*: compile time (constant-folded into
  the kernel), launch time (the runtime picks geometry), or source (the kernel
  asks the profile). The picker exists and is advisory; making it decide is a
  design choice with a blast radius.
- Whether a kernel can be written **parametrically** at all in today's language
  — `Wave.width()` is a per-target const expression, but a tile size that must
  fold to a constant for LDS declaration is a harder case.

**About the two machines**
- Does cajeta-llm run on nvptx today, and what does a decode step cost on a
  4090?
- Which of the 183 launch shapes encode a wave width versus merely pick a block
  size?
- Which arc findings survive Ada, which invert (the launch-overhead question of
  §8), and which were RDNA3 issue-rate facts that never generalized?
- What is the parity criterion across vendors, given that the arc's standing
  gate is exact token equality?

**About the boundary**
- Which of §5.2's units move, and to where — and does the general optimizer
  itself belong in the compiler, the stdlib, or a new home?

---

## 11. Provenance

Repositories pulled 2026-09-02 into `/home/julian/code/cpp/`: `cajeta`
(`407dd5f6`), `cajeta-ml` (`409cdc2`), `cajeta-cabra` (`ad0f5df`), `cajeta-llm`
(`7191a48` → `0840d8e`, 51 commits, +19,065/−720 over 89 files), plus
`cajeta-unit`, `cajeta-codec`, `cajeta-jinja`, `cajeta-logging`, `cajeta-http`.
The `agents/` plan repo (`cajeta-agents`, gitignored inside `cajeta`) was
missing entirely from this clone and is now installed at `cajeta/agents`
(`a98a088`).

**Two naming/branch hazards found while doing that**, both live:

1. `cajeta-llama` and `cajeta-llm` are the **same GitHub repo** (identical HEAD
   through both URLs); the rename left `origin/cajeta-llama/unit-13-chat` and
   `origin/cajeta-llm/unit-13-chat` both standing, and
   `tools/ownership/harvest_*.sh` still names the old one in prose while its
   `CAJETA_HARVEST_LIBS` default list omits the project under either name.
2. **The live llm plan is not on `agents` `main`.** `main` carries
   `cajeta-llama-plan.md` at its 19-unit state; the plan that records units
   45–61 and Julian's direction block is `cajeta-llm-plan.md` on
   `origin/worktree-cajeta-llama-unit-1`, and the two branches have diverged by
   55 files (+11,797/−13,402) — `main` has plans that branch lacks
   (`xpu-cache-discriminator`, `lambda-frame-line`, `profile-run-history`) and
   vice versa (`stdlib-ownership-convention`, `threaded-forward-path`,
   `simd-fused-integer-madd`). `specs/INDEX.md` links `../agents/cajeta-llm-plan.md`,
   which **does not exist on `main`**. Where this work's plan is written, and
   which branch is authoritative, is a decision for Julian and is Q9 in the
   research plan.
