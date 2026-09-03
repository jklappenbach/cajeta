# Spec: Kernel artifact inspection (`kernel-artifact-inspection`)

**Filed 2026-09-02**, split out of `hardware-profile-tuning-research-plan`
Unit 0 at Julian's direction ("Yes on 0.2.3") because it is a new instrument,
not a fix, and it gates tuning work on any backend where it does not exist.

## 1. Definition

### 1.1 Purpose — what wall-clock cannot see

A profiler answers *where did the time go*. It cannot answer *why*, because the
two failure modes that most often make a hand-tuned kernel slow — a **register
spill** and a **serialized load chain** — are invisible in a duration. They are
visible in the artifact the shader compiler produced, and only there.

This is not a hypothetical. The AMD arc's two most valuable findings both came
from reading the code object, not from timing:

- *"the k-quant kernels were **ALU-bound, not DRAM-bound**"* (cajeta-llm unit
  60) — a conclusion that reversed the tuning strategy for the whole decode
  path, and which the wall-clock had been consistent with for weeks.
- a run of single loads each behind `s_waitcnt vmcnt(0)`, worth **205 → 224
  GB/s** on the q6k head (unit 58).

That instrument is `cajeta-llm/src/main/cajeta/dev/cajeta/llm/bench/KernelIsa.cajeta`
(236 lines). It carves the AMD code objects out of a built executable, reports
the kernel descriptor via `llvm-readelf`, and disassembles via `llvm-objdump`.
It exists for exactly one vendor, and it lives in an application repository.

**The requirement is instrument parity**: the same questions, answered in the
same columns, on every backend cajeta emits device code for — so that a number
from one machine can be set beside a number from another and mean the same
thing.

### 1.2 Scope

- **In:** (a) acquiring the device code from what actually shipped;
  (b) a **per-kernel resource report** in one schema across backends —
  registers, spills, scratch, shared/LDS, and the occupancy-limiting resource;
  (c) an optional **disassembly tier** with instruction-class counts;
  (d) identification of the **toolchain and target arch** that produced each
  artifact; (e) an NVIDIA provider, which does not exist today.
- **Out:** timing of any kind (that is `cajeta-profiler`); choosing a launch
  configuration from what is read (that is `xpu-device-profile`'s picker and
  the hardware-profile work); modifying the artifact; vendor profilers that
  need a privileged gate (`ncu`, `rocprof`) — this reads a file.

### 1.3 Non-goals

- Not a replacement for `cajeta profile summary`. Resource facts and time are
  different questions and both are needed; conflating them is how "the kernel
  got slower" becomes unattributable.
- Not a simulator. It reports what the shader compiler emitted, not what the
  hardware will do with it.
- Not a per-vendor tool wearing a portable name. If a fact exists on one
  backend and not another, the report **says so** rather than printing a zero.

### 1.4 Principles

- **Read what shipped.** The subject is the artifact inside the built binary,
  not a side-channel emission that may have been produced differently. A
  separate emission path is a convenience, never the source of truth.
- **One schema, honest holes.** Every backend fills the same columns or
  declares a column unavailable. A missing fact is never rendered as `0`.
- **Name the toolchain.** Every report states which assembler produced the
  artifact and which reader read it. Version skew between the two is the first
  thing to suspect and the cheapest thing to record.
- **Clear the staging area first, always.** `cajeta-llm` commit `cfdf243`
  records a stale `.s` from an earlier filter reading as the current build
  **for an hour**. Staleness in an instrument is worse than its absence.
- **Acceptance is a criterion, not a dump.** `Spilled/scratch 0` is the bar the
  AMD arc actually gated on; the report exists to make that checkable.

---

## 2. Acquisition — getting the device code out of what shipped

### 2.1 Requirement

Given a cajeta-built executable, the inspector shall recover every embedded
device code object, attributed to the kernel that owns it, without depending on
symbol names surviving into the final binary.

### 2.2 Mechanism — a structural carve, verified on both vendors

Both vendors' code objects are ELF, so both are found by scanning the host
binary for ELF headers and filtering on `e_machine`:

| backend | `e_machine` | verified |
|---|---|---|
| AMD | `EM_AMDGPU` = 224 | shipped — `KernelIsa.cajeta` does exactly this |
| NVIDIA | `EM_CUDA` = **190 (0xBE)** | **MEASURED 2026-09-02** on a `ptxas`-produced sm_89 cubin |

The NVIDIA target architecture is **not** safely recoverable by hand-parsing
`e_flags` — a rule that held under one toolchain and broke under the next,
caught 2026-09-02 before any code was written:

| producer | `e_flags` | low byte | byte 1 |
|---|---|---|---|
| `ptxas` 12.0 | `0x00590559` | **89** ✓ | 5 |
| `ptxas` 13.3 | `0x06005904` | 4 ✗ | **89** ✓ |

The SM byte moved. A reader written against 12.0 reports **sm_4** for a 13.3
cubin — a wrong answer, confidently rendered, which is the one failure mode
this instrument exists to prevent. So:

> **The arch is read from the disassembler's own output, never from `e_flags`.**
> `e_flags` may be recorded as a cross-check and must never be the source.

The disassembler's spelling **also** changed and the parse must accept both:

| producer | `nvdisasm -c` says |
|---|---|
| 12.0 | `.headerflags @"… EF_CUDA_SM89 EF_CUDA_VIRTUAL_SM(EF_CUDA_SM89)"` |
| 13.3 | `.target sm_89` |

(`cuobjdump -elf` is not a substitute: it surfaced the arch for the 13.3 cubin
only, inside a `Tool Command Line Arguments:` line, and nothing greppable for
the 12.0 one.)

Reading the arch at all is load-bearing beyond curiosity — it makes an artifact
carrying the *wrong* architecture's code visible by inspection, which is
precisely the failure `xpu-cache-discriminator` produces silently today.

**What IS stable across 12.0 → 13.3: `e_machine` = 190.** The carve premise
holds; only the arch decode was version-fragile.

#### 2.2.1 The extent formula (**MEASURED — and it is not the obvious one**)

An embedded object's length is not recorded anywhere the carve can read (the
host stores it, but only as an argument to a ctor call). It must be computed
from the object's own headers, and the naive computation **silently
truncates**:

> `extent = max(e_phoff + e_phnum·e_phentsize,`
> `            e_shoff + e_shnum·e_shentsize,`
> `            max(sh_offset + sh_size) over non-NOBITS sections,`
> `            max(p_offset + p_filesz) over segments)`

The term that matters is the **first**: in a cubin the **program header table
sits at the END of the file**. Measured on `KernelProfile.saxpy.cubin` —
`e_phoff` = 3200 with 3 × 56-byte entries = **3368** = the true file size,
while the section-table computation stops at 3200. Two successive attempts here
that omitted it produced 3200-byte files that looked entirely plausible and
were rejected by every reader with *"does not contain device code"*.

#### 2.2.2 Validation against ground truth (**MEASURED 2026-09-02**)

`samples/kernel-profile` built with `--xpu-backend=nvptx --xpu-arch=sm_89`,
then carved from the resulting 1.33 MB executable:

| carved | size | vs `--xpu-emit=cubin` output |
|---|---|---|
| #00 | 3368 | **sha256-identical** to `KernelProfile.saxpy.cubin` |
| #01 | 3240 | **sha256-identical** to `KernelProfile.scale.cubin` |
| #02 | 3368 | **sha256-identical** to `KernelProfile.vecAdd.cubin` |

3 of 3, byte for byte. Each then read with the 13.3 tools directly off the
carved bytes:

| kernel (from the cubin's own symbols) | arch | REG | STACK | SHARED | LOCAL |
|---|---|---|---|---|---|
| `saxpy` | sm_89 | 10 | 0 | 0 | 0 |
| `scale` | sm_89 | 8 | 0 | 0 | 0 |
| `vecAdd` | sm_89 | 12 | 0 | 0 | 0 |

**`Spilled/scratch 0` is therefore checkable on Ada as of today** — the
acceptance criterion the AMD arc gated on, which is what this instrument was
filed to provide.

A consequence for §1.4's principle: because the embedded and emitted objects
are byte-identical, `--xpu-emit` is a legitimate **cross-check** on the carve,
not merely a convenience. The carve stays the source of truth; the emission is
now a way to prove the carve correct, and Unit 1 uses it exactly that way.

How the bytes get there, on the NVIDIA side (`NvptxRegistration.cpp:112-135`):
each kernel's cubin is embedded as a **private** host-module constant named
`xpu.cubin.<entry>`, 8-byte aligned, registered by a ctor calling
`__cajeta_xpu_register_module(name, ptr, len, CAJ_XPU_CUDA)`. Private linkage
is exactly why the carve must be structural — those names need not survive.
Kernel attribution then comes from **inside** the cubin, whose own symbol table
carries the entry names.

A second acquisition path exists and is specified as a convenience, not the
source of truth: `--xpu-emit=ptx|cubin` (NVIDIA) and `--xpu-emit=isa|hsaco`
(AMD) write device code to disk at build time (`Compiler.cpp:3437-3451`).

### 2.3 Use cases

- 2.3.1 As a developer with a built executable, when I run the inspector, then
  every embedded kernel is listed with its target architecture, with no build
  flags required and no rebuild.
- 2.3.2 As a developer whose binary was built for one backend and silently
  cached from another, when I inspect it, then the architecture reported per
  object is the one actually embedded — so the discrepancy is visible.
- 2.3.3 As a developer on a binary with no device code, when I inspect it, then
  I am told that plainly and the exit status distinguishes it from a failure.
- 2.3.4 As a developer inspecting a multi-arch artifact, when I run the
  inspector, then each architecture's object is reported separately rather than
  the first one standing for all.

---

## 3. The report — one schema across backends

### 3.1 Requirement

For every kernel, the inspector shall report the resources the shader compiler
committed to it, in one schema, with any column a backend cannot supply marked
**unavailable** rather than zero.

Columns: kernel · target arch · registers/thread · **spill stores / spill
loads** · scratch or stack bytes · shared/LDS bytes · constant bytes ·
occupancy-limiting resource.

### 3.2 Per-backend provider mapping

| column | AMD (shipped) | NVIDIA (**MEASURED** available) |
|---|---|---|
| registers | VGPR/SGPR from the kernel descriptor (`llvm-readelf`) | `REG:` from `cuobjdump -res-usage`; also `SHI_REGISTERS=` in `nvdisasm -c` output |
| spills | descriptor spill counts | `STACK:` and `LOCAL:` |
| scratch | descriptor scratch | `STACK:` |
| shared | LDS bytes | `SHARED:` |
| constant | — | `CONSTANT[0]:` |
| occupancy limiter | derived from `DeviceProfile.occupancy()` | same, once the NVIDIA device model exists (`xpu-device-profile`, and the model is `arch:"unknown"` on Ada today) |

Verified sample, `nvcc -arch=sm_89 -cubin` on this box:
`REG:23 STACK:0 SHARED:0 LOCAL:0 CONSTANT[0]:372`.

### 3.3 Use cases

- 3.3.1 As a kernel author, when I inspect my kernel, then I can check
  `spills == 0` as an acceptance criterion without reading disassembly.
- 3.3.2 As a developer comparing two machines, when I inspect the same kernel
  on each, then the columns line up and a spill on one is visible against the
  other.
- 3.3.3 As a developer whose backend cannot report a column, when I inspect,
  then that cell reads *unavailable* and names why — never `0`.
- 3.3.4 As a developer, when the register count changes between two builds of
  the same source, then the report makes it visible without a device run.

---

## 4. Disassembly tier (opt-in)

### 4.1 Requirement

On request, the inspector shall disassemble a kernel and report instruction
counts by class — memory operations, barriers/waits, and the arithmetic classes
that distinguish an ALU-bound kernel from a memory-bound one.

### 4.2 Mechanism

AMD: `llvm-objdump`, counting memory and `s_waitcnt` instructions (shipped).
NVIDIA: `nvdisasm -c` (**MEASURED** working on this box; `LDG/STG/LDS/STS/BAR`
counting verified). The vendor-specific classes are named in a per-backend
table, because *"count the loads"* means different opcodes on each.

### 4.3 Use cases

- 4.3.1 As a developer who suspects a serialized load chain, when I disassemble,
  then I see the wait/barrier instructions and their density.
- 4.3.2 As a developer deciding whether a kernel is ALU- or memory-bound, when
  I read the class counts alongside the profiler's time, then I have both
  halves of unit 60's argument.

---

## 5. Toolchain identification and version safety

### 5.1 Requirement

Every report shall name the toolchain that **produced** the artifact and the
one that **read** it, and shall warn when they differ in a way that can change
the answer.

### 5.2 The live trap on this machine (**MEASURED 2026-09-02**)

The trap this section was written against **fired the same day, twice**, which
is the argument for the requirement.

*Before:* `nvidia-smi` reported CUDA 13.3 while the only compiler toolchain was
**12.0** (`/usr/bin/{ptxas,nvdisasm,cuobjdump}` at `V12.0.140`) and
`/usr/local/cuda-13.3` had no `bin`. Driver version ≠ toolchain version.

*After* installing `cuda-nvcc-13-3` / `-cuobjdump-` / `-nvdisasm-`
(`V13.3.73` under `/usr/local/cuda/bin`): **both toolchains are now present**,
and which one runs depends on how the caller looks. `findPtxas()` checks
`$CUDA_PATH/bin/ptxas` **first**, so cajeta compiles with **13.3**; bare
`ptxas` on `PATH` still resolves `/usr/bin/ptxas`, i.e. **12.0**. A report
produced by reading a 13.3 cubin with a 12.0 `nvdisasm` is exactly the skew
§5.1 requires be declared — and §2.2 shows it is not academic: the two
disagree on both the `e_flags` layout and the disassembly spelling.

Verified direction of compatibility: a **13.3 reader reads a 12.0 cubin**
correctly (`cuobjdump -res-usage` gave identical output for both). The reverse
is the untested and riskier direction.

### 5.3 Use cases

- 5.3.1 As a developer, when I read a report, then I know which `ptxas`
  produced the code and which `nvdisasm` read it.
- 5.3.2 As a developer whose reader is older than the producer, when I inspect,
  then I am warned rather than shown a plausible wrong answer.
- 5.3.3 As a developer with several toolchains installed, when I inspect, then
  the one used is the one that built the artifact, or the mismatch is stated.

---

## 6. Failure modes the design must not have

- **Silent staleness.** The staging directory is cleared before every run, and
  the report states the artifact's own hash (`cfdf243`'s hour).
- **A zero that means "unknown".** Every unavailable fact says so.
- **A single-arch answer for a multi-arch artifact** (§2.3.4).
- **Passing when nothing was inspected.** A run that found no device code exits
  non-zero — the same discipline as `device-tests.yml`'s "fail if nothing
  actually ran on hardware" step, which exists because a green suite once ran
  no CUDA kernel at all.

---

## 7. Where it lives

`KernelIsa` is in `cajeta-llm/bench/`, an application repository, while being
toolchain material by nature — it inspects compiler output and knows nothing
about language models. `hardware-profile-tuning-findings.md` §5.2 lists it as a
module-responsibility candidate.

This spec asserts the destination and defers the move: the inspector is a
**toolchain** capability. Whether it lands as a `cajeta` CLI verb, a tool under
`tools/`, or a stdlib-adjacent library is §8's first open question. The NVIDIA
provider must not be written into `cajeta-llm`, which would double the debt.

---

## 8. Open questions

- **8.1 Home and surface.** A `cajeta` CLI verb (`cajeta kernel-isa <exe>`), a
  script under `tools/`, or a cajeta program as KernelIsa is today
  (`5c1b737`: *"plan: KernelIsa is cajeta"* — a deliberate choice worth
  preserving)? The answer decides how the AMD half migrates.
- **8.2 Does the AMD half migrate in this cycle**, or does the NVIDIA provider
  land beside it and the merge follow? Migrating first is cleaner and delays
  the instrument the tuning work is blocked on.
- **8.3 Vulkan and CPU providers.** SPIR-V has no register concept and the CPU
  backend's "kernel" is host code. Do they get *unavailable* rows, or is the
  inspector defined as native-backend-only?
- **8.4 Is `--xpu-emit` promoted?** It already writes ptx/cubin/isa/hsaco. If
  the carve is the source of truth, does `--xpu-emit` stay a debug convenience
  or become the supported acquisition path for CI?
- **8.5 Occupancy column.** It needs `DeviceProfile`, which reports
  `arch:"unknown"` on Ada. Does this spec depend on that fix, or ship the
  column as unavailable until it lands?
