# XPU kernel adaptor — spec

Status: **draft** 2026-09-18, amended 2026-09-19 with §1.4, §3.5-3.7
and §7 after the first run on a second vendor. Registered in
[INDEX.md](INDEX.md).
Layer: `cajeta.xpu`. **Supersedes `device-geometry-parameterization`**
(draft 2026-09-03, never registered, no plan). That draft made the
device half queryable, which is one of the three parts below. Its §2
geometry tiers and §2.4 frozen-site inventory are absorbed here.

## 1. Definition

Three questions stand between a kernel and a device it has never run on.
**What does the kernel need**, meaning the shape and compute facts only
its author knows. **What does the device offer**, meaning the profile.
**What configuration satisfies both**, meaning the reconciliation.

The middle one is built and closed. The other two do not exist, so a
kernel is written against one part and frozen there.

Julian's north star, stated 2026-09-02 and carried from the superseded
draft:

> Ideally, we have one code base in the cajeta-llm project, and it works
> maximally optimized no matter what device it's running on.

### 1.1 Problem statement

Measured, each with its source.

- **The reconciler is called, and it is wired to the wrong model.**
  Corrected 2026-09-19. The original claim here was that nothing called
  it, and that is false. `DeviceProfile.h` ships `occupancy`,
  `occupancyLimiterName` (which budget binds), `candidateBlocks`
  (wave-multiple, best occupancy first), `classifyBound`, and
  `LaunchPick{block, occupancyWaves, bound, geometryWontHelp,
  advisoryOnly, needsSweep}`. `KernelManifest.cpp:223` calls them and
  writes `feasibleBlocks` and `occupancyLimiter` into each kernel's
  manifest, which `KernelManifest.of(name)` already exposes to cajeta
  source. What it cannot do is answer for a device the arch table has
  never seen. `fillOccupancy` opens with
  `if (!lookupArch(archName, model)) return;` and `kArchTable` holds two
  rows, `gfx1151` and `gfx1100`. Measured on an RTX 4090,
  `lookupArch("sm_89")` is NOT FOUND, so every NVIDIA kernel manifest
  carries `occupancyLimiter` ABSENT and `hasFeasibleBlocks` false. It is
  still a thing you can print rather than a thing that decides, but the
  cause is the compile-time model, not a missing caller.
- **A kernel can declare almost nothing.** The whole vocabulary is
  `@Kernel`, `@Occupancy(maxThreads)` and `@FastMath`. There is no way
  to say "any wave width", "I need dp4a", "my tile is 64x64", or "this
  much LDS". So the reconciler could not be called correctly even if
  something called it. `occupancy()` is handed a measured VGPR count and
  no statement of what the body assumes.
- **The cost, in the kernels.** `QuantKernel.cajeta` has **135** sites
  keyed to a literal 32 and reads `Wave.width()` **zero** times.
  `Device.waveSize()` answers **0 on the CPU backend**. The backend
  computes the width for the Vector Function ABI
  (`CpuRegistration.cpp:56`) and never publishes it to the geometry
  surface.
- **The cost, in a stranded kernel.** MXFP4 carries five kernels and
  seven launchers. The fastest one decodes with `lut4` to int8 and then
  dots with `dotSum`, measured at 43 us against llama.cpp's 58.4. It is
  called only from `Mxfp4Bench`, because its own doc says "wave width 32
  ONLY" and nothing in the system can express or satisfy that. The
  portable kernel is what production reaches, through a fallback arm.
- **Capability is not data.** Every "can this device do X" fact is
  expressed as whether a `LoweringTarget` subclass overrides a virtual
  method. None of it reaches `DeviceProfile` and none is queryable from
  cajeta source (findings §7.4).

### 1.2 Scope

- The declaration vocabulary on a kernel.
- The profile made reachable and authoritative at run time.
- The adaptor that reconciles the two at BIND, answering with a launch
  configuration or a named refusal.
- Adaptation by launch geometry and specialization constants.
- A bounded empirical search where the model says the device is
  unmodelable.

### 1.3 Non-goals

- **Ranking the survivors.** When several configurations or variants
  satisfy a device, choosing the fastest belongs to the cost model. That
  is the tile manifest's measured expressions, the scheduler's ridge and
  `Autotune`. This spec produces correct candidates. It does not order
  them.
- **Choosing among data formats.** That is the route table. No
  adaptation turns a Q4_K decoder into an IQ3_XXS one.
- **Self-tuning across runs.** Recording a winner and improving on it
  between processes is the hardware-profile research plan's Q5.
- **Writing kernels.** The adaptor configures a kernel. It does not
  author one.

### 1.4 The maintenance constraint

The north star has a corollary that governs every choice below. Julian,
2026-09-19, and load-bearing rather than aspirational:

> Supporting a device we have never seen must cost no per-device source.

A design that answers "what block size" from a table keyed by
architecture satisfies the north star on the parts already in the table
and fails it on every part that ships afterwards.

llama.cpp is the worked example of the alternative and the reason to
refuse it. Its decode path picks `nwarps` from a seven-way
`mmvq_parameter_table_id`, and its prefill path carries NINE
per-architecture config headers holding 2027 `CASE` rows over 22 quant
types, measured 2026-09-19. Ada and gfx1151 disagree in every cell that
exists for both. Each of those rows was fitted by hand and has to be
refitted per part. That is the price of baking configuration at compile
time with no runtime model to consult, and it is the artifact this
layer exists to make unnecessary. If cajeta needs its own copy of that
table, the language has no portability answer worth the name.

cajeta has the runtime model, and it already clears the bar. The same
measurement that found `lookupArch("sm_89")` NOT FOUND also found
`cajeta gpu-profile` answering `estimated:false` with every value live
on that part. `buildDeviceModel` says why:

    liveOccupancy = props.regsPerMP && props.threadsPerMP;
    m.estimated = !(props.valid && (archKnown || liveOccupancy));

A device the table has never seen is fully modelled when the driver
reports registers per MP and threads per MP. The table is therefore not
an enumeration of supported hardware. It is a fallback for a query that
does not answer, which is why both its rows are AMD.

- **1.4.1** When a device the tree has never seen is attached, it is
  modelled from the driver and no source changes.
- **1.4.2** When a quantity is derivable from a driver attribute, it is
  derived rather than tabled.
- **1.4.3** When a quantity is not derivable, it is keyed by FAMILY
  rather than by part, and the family test is a reported value or a
  prefix rather than an enumeration. `cajeta_xpu_simds_per_mp` is the
  shape to copy. It prefers what the device reported, then matches
  `sm_` or `gfx`, then answers 0.
- **1.4.4** When a quantity is neither derivable nor a family constant,
  it is MEASURED per machine and recorded by `Autotune`, never typed
  into a source file. `wavesPerSimdTarget` is the only such value in
  the tree today, and its own comment already concedes the point.
- **1.4.5** When a family constant has no answer for the attached part,
  the profile says unknown and the dependent derivation refuses, rather
  than substituting a default that is right elsewhere. `simdsPerMP`
  silently defaults to 4 today, which is correct on Ada and on RDNA3.5
  by coincidence and unchecked on anything else.

The four sections that follow are that constraint decomposed. The
device describes itself (§3). The kernel declares only what no compiler
can infer from its body (§2). The adaptor reconciles the two at bind
(§4). Measurement fills the residue and nothing else (§6).

## 2. The kernel declares what only its author knows

An author declares what only an author knows. Everything else is
measured or derived from the body. A code object reveals that a kernel
burns 84 VGPRs. It cannot reveal that the body assumes 32 lanes.

- **2.1** When a kernel's correctness depends on a device quantity, it
  declares that dependence, and the declaration is on the KERNEL rather
  than on any caller or route.
- **2.2** When a kernel is agnostic to a quantity, it says so, and the
  adaptor is free to choose.
- **2.3** When a declaration and the body disagree, the compiler says
  so. A kernel that declares itself wave-agnostic and hard-codes a lane
  count does not build. A declaration that is merely trusted is a
  comment.
- **2.4** When the cost of a kernel is wanted, it is measured or derived
  from the body, never declared. The author's estimate of their own
  kernel's speed rots on the next device (xpu-tile-manifest, resolved
  2026-09-06).
- **2.5** When a new declaration is added, existing kernels keep
  building. The vocabulary is additive, and an undeclared quantity means
  "no constraint stated", not "no constraint".
- **2.6** When a declaration names a capability, it names a fact about
  the device, not a backend.

## 3. The profile decides, rather than prints

- **3.1** When a launch is being configured, the live device profile is
  readable from the runtime, not only from the CLI.
- **3.2** When the profile cannot answer a question, it says so rather
  than guessing. `dispatchBlocks` already returns 0 and never a guess,
  and `classifyBound` already answers `Unknown` without a ceiling.
- **3.3** When the CPU backend is active, it reports the wave width it
  already computes, rather than 0.
- **3.4** When a device capability is queried, it is a fact in the
  profile, not the shape of a C++ vtable.
- **3.5** When the arch table has no row for the attached device, the
  live model still answers and the occupancy surface is computed from
  it. A compile-time fill may stand as an AOT hint, and it is not the
  authority at bind.
- **3.6** When a driver attribute and a table constant disagree, the
  driver wins. `buildDeviceModel` already states that rule for the
  runtime model, and the compile-time path does not inherit it.
- **3.7** When a predicate answers whether the device can run a named
  kernel, it consults the registry the LAUNCH consults. A predicate
  that answers from a different table is worse than no predicate,
  because a caller routes on it. Measured 2026-09-19:
  `Device.kernelAvailable("q4kF16CoopN256Kernel")` answered true in the
  same process whose launch then failed with `no registered kernel`.

## 4. The adaptor reconciles, at bind

- **4.1** When a kernel is bound, the adaptor reconciles its declaration
  against the live profile ONCE and the result is stored. The launch is
  then a configured dispatch, not a negotiation.
- **4.2** When reconciliation succeeds, the result is a launch
  configuration: block, grid, and the specialization constants of §5.
- **4.3** When reconciliation fails, the adaptor REFUSES BY NAME. It
  names the declared quantity that could not be met, or the budget that
  bound the occupancy, which `occupancyLimiterName` already answers.
- **4.4** When a variant is refused, the route table's next row serves
  the work. This is what makes "every format runs on the host" a
  structural property rather than a promise. The refusal is what selects
  the fallback.
- **4.5** When geometry cannot help, because the kernel is memory-bound
  and already at the roofline, the adaptor says so rather than
  searching. `LaunchPick.geometryWontHelp` is that answer and it already
  exists.

## 5. Adaptation: geometry and specialization constants

- **5.1** When a launch shape is derivable from the profile, it is
  derived and not frozen. The superseded draft's §2.4 inventory of
  frozen sites is the work list.
- **5.2** When a kernel parameter is a device quantity the body can be
  specialized on, such as wave width, tile extent or unroll, it is bound
  as a specialization constant at load rather than compiled in.
- **5.3** When two kernels differ ONLY by a device quantity, they become
  one kernel with that quantity specialized. MXFP4's coop and portable
  pair is the reference case.
- **5.4** When a kernel is specialized per device, the specialization is
  keyed and cached, so a second bind on the same device does not repeat
  it.

## 6. Bounded search, inside the adaptor

Search is the adaptor's third tier, not a separate system. The ordering
is the one the hardware-profile research recorded. Analytical first,
guidance second, search last and bounded. An analytical model reaches
94.7% of exhaustive autotuning with tuning time reduced to zero
(tritonBLAS, findings §9.1), so search is what runs where the model
declines to answer.

- **6.1** When the profile marks a device unmodelable, and
  `LaunchPick.needsSweep` is already that signal, the adaptor verifies
  its choice empirically instead of trusting it.
- **6.2** When a search runs, it is BOUNDED. It has a stated budget of
  configurations and of time, never an open sweep.
- **6.3** When a search runs, its space contains the configuration the
  analytical model chose, so the search can never return something worse
  than where it started. hipBLASLt's documented failure mode is the
  reason this is a requirement: "only as good as the search space you
  hand it".
- **6.4** When a search produces a winner, it is recorded against the
  device AND the kernel set that was measured, so a rebuild invalidates
  it rather than silently serving a stale answer. `Autotune.rememberFor`
  and `recallFor` already carry the `buildId` this needs, and discard
  and report a hint written for different code, so the adaptor adopts
  that store rather than adding one.
- **6.5** When a search would cost the caller live work, it does not run
  on the critical path uninvited.

## 7. Measured on a second vendor

Recorded 2026-09-19 on PHOENIX, RTX 4090, sm_89, driver 610.62, CUDA
13.3, against cajeta main 44a3daa3 and cajeta-llm main 20d8d86. The
tables are in `nvidia-kernel-geometry-findings.html` beside this file.
Everything above is answerable to these.

- **The runtime half already satisfies 1.4.1.** `sm_89` is absent from
  the arch table and is modelled anyway. 128 SMs, wave 32, 48 waves per
  MP, 24 resident blocks per MP, 49152 bytes of shared memory per block
  with a 101376 opt-in, roofline measured at 859.2 GB/s against a
  theoretical 1008.1, `estimated:false`.
- **The frozen half is 161 of 162.** `QuantKernel.cajeta` carries 162
  launch sites. 86 pass `block: [256]`, 43 pass `block: [32]`, 20 pass
  a named constant of 64, 7 pass 128, 5 pass 64, and exactly ONE
  derives the block from the device. The file reads `Device` twice in
  18912 lines and `Wave.width()` zero times.
- **The same block size is optimal on one part and half rate on the
  other.** Through the shipped `occupancy()`, a 32-thread block reaches
  24 of 48 waves per MP on Ada and 64 of 64 on gfx1151. The binding
  limit is Ada's reported cap of 24 resident blocks per MP, which
  gfx1151 does not report at all. A 64-thread block reaches 48 of 48.
  This is §5.1 with a number on it, and no benchmark was needed to see
  it because the driver already reports the cap.
- **The grid is sized for the smaller part.** The same Q4_K wave
  mat-vec moving the same 31.5 MB achieves 348.8 GB/s at 14336 output
  rows and 48.4 GB/s at 4096, because the launcher issues 3584
  workgroups in the first case and 1024 in the second against 3072 warp
  slots reachable with one-warp blocks. On gfx1151 both counts fill the
  part, which is why the geometry was never wrong there.
- **A failed launch is silent.** Five of six Q4_K coop variants do not
  launch on nvptx, including both that the Auto router selects, and the
  output buffer is left zeroed rather than refused. 54 of the 58 coop
  kernels have no manifest at all, which also makes `tuneBuildId`
  answer `none` for them and any `Autotune` hint unrecallable. This is
  a §4.3 problem before it is a tuning problem, and §3.7 is the
  predicate half of it.
- **There is no trustworthy per-kernel device timer on this part.** The
  profiler's GPU tier produced no device queue track, CUPTI is
  unavailable, and `KernelIsa` is AMD-only. A harness timing the one
  coop variant that does run implied 252 TFLOPS against the part's
  roughly 165 TFLOPS dense f16 peak, so it is measuring something other
  than execution. §6 cannot be falsified until that is fixed, which
  makes the timer a prerequisite for the search tier rather than a
  convenience.

## 8. What follows

- Ranking survivors by measured cost. This is the tile family's, and it
  is what finally retires `Route.priority()`, a declared integer
  standing where a measurement belongs.
- Self-tuning between runs, the research plan's Q5. Its premise was that
  one bandwidth-bound GEMM generalized to "no real workload", and the
  ALU-bound k-quant decode kernels have already undercut that.
- Fleet calibration, meaning a recorded winner shared across nodes
  rather than rediscovered per process. It is named as the gap in the
  route-table spec's §7 and owned by nothing.
- A per-kernel device timer on NVIDIA. §7 records that there is none,
  and §6's search tier cannot be accepted or rejected without one. It
  is the NVIDIA half of `kernel-artifact-inspection`, which already
  filed the symmetric AMD instrument, and it gates this spec's last
  tier rather than following from it.
- Why 54 of 58 coop kernels produce no device code on nvptx. The
  adaptor reconciles kernels that exist. A kernel that silently fails
  to lower is upstream of everything here, and the compiler reporting
  it is a diagnostic-engine concern rather than an adaptor one.
