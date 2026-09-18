# XPU kernel adaptor — spec

Status: **draft** 2026-09-18. Registered in [INDEX.md](INDEX.md).
Layer: `cajeta.xpu`. **Supersedes
`device-geometry-parameterization`** (draft 2026-09-03, never registered,
no plan), whose §2 geometry tiers and §2.4 frozen-site inventory are
absorbed here: it made the device half queryable, which is one of the
three parts below.

## 1. Definition

Three questions stand between a kernel and a device it has never run on.
**What does the kernel need** — the shape and compute facts only its
author knows. **What does the device offer** — the profile. **What
configuration satisfies both** — the reconciliation.

The middle one is built and closed. The other two do not exist, so a
kernel is written against one part and frozen there.

Julian's north star, stated 2026-09-02 and carried from the superseded
draft:

> Ideally, we have one code base in the cajeta-llm project, and it works
> maximally optimized no matter what device it's running on.

### 1.1 Problem statement

Measured, each with its source.

- **The reconciler exists and nothing calls it.** `DeviceProfile.h`
  ships `occupancy`, `occupancyLimiterName` (which budget binds),
  `candidateBlocks` (wave-multiple, best-occupancy first),
  `classifyBound`, and `LaunchPick{block, occupancyWaves, bound,
  geometryWontHelp, advisoryOnly, needsSweep}`. `DeviceProfile` /
  `queryLiveDeviceModel` appear in FOUR files: `main.cpp`, the profile's
  own `.h`/`.cpp`, and the CLI command. It is a thing you can print, not
  a thing that decides (hardware-profile findings §7.2).
- **A kernel can declare almost nothing.** The whole vocabulary is
  `@Kernel`, `@Occupancy(maxThreads)` and `@FastMath`. There is no way
  to say "any wave width", "I need dp4a", "my tile is 64x64", "this
  much LDS". So the reconciler could not be called correctly even if
  something called it: `occupancy()` is handed a measured VGPR count and
  no statement of what the body assumes.
- **The cost, in the kernels.** `QuantKernel.cajeta` has **135** sites
  keyed to a literal 32 and reads `Wave.width()` **zero** times.
  `Device.waveSize()` answers **0 on the CPU backend**, because the
  backend computes the width for the Vector Function ABI
  (`CpuRegistration.cpp:56`) and never publishes it to the geometry
  surface.
- **The cost, in a stranded kernel.** MXFP4 carries five kernels and
  seven launchers. The fastest — `lut4` to int8 then `dotSum`, measured
  43 us against llama.cpp's 58.4 — is called only from `Mxfp4Bench`,
  because its own doc says "wave width 32 ONLY" and nothing in the
  system can express or satisfy that. The portable one is what
  production reaches, through a fallback arm.
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
  satisfy a device, choosing the fastest is the cost model's — the tile
  manifest's measured expressions, the scheduler's ridge, `Autotune`.
  This spec produces correct candidates. It does not order them.
- **Choosing among data formats.** That is the route table. No
  adaptation turns a Q4_K decoder into an IQ3_XXS one.
- **Self-tuning across runs.** Recording a winner and improving on it
  between processes is the hardware-profile research plan's Q5.
- **Writing kernels.** The adaptor configures a kernel. It does not
  author one.

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
  so — a kernel that declares itself wave-agnostic and hard-codes a lane
  count does not build. A declaration that is merely trusted is a
  comment.
- **2.4** When the cost of a kernel is wanted, it is measured or derived
  from the body, never declared. The author's estimate of their own
  kernel's speed rots on the next device (xpu-tile-manifest, resolved
  2026-09-06).
- **2.5** When a new declaration is added, existing kernels keep
  building — the vocabulary is additive, and an undeclared quantity
  means "no constraint stated", not "no constraint".
- **2.6** When a declaration names a capability, it names a fact about
  the device, not a backend.

## 3. The profile decides, rather than prints

- **3.1** When a launch is being configured, the live device profile is
  readable from the runtime, not only from the CLI.
- **3.2** When the profile cannot answer a question, it says so rather
  than guessing — `dispatchBlocks` already returns 0 and never a guess,
  and `classifyBound` already answers `Unknown` without a ceiling.
- **3.3** When the CPU backend is active, it reports the wave width it
  already computes, rather than 0.
- **3.4** When a device capability is queried, it is a fact in the
  profile, not the shape of a C++ vtable.

## 4. The adaptor reconciles, at bind

- **4.1** When a kernel is bound, the adaptor reconciles its
  declaration against the live profile ONCE and the result is stored —
  the launch is then a configured dispatch, not a negotiation.
- **4.2** When reconciliation succeeds, the result is a launch
  configuration: block, grid, and the specialization constants of §5.
- **4.3** When reconciliation fails, the adaptor REFUSES BY NAME — the
  declared quantity that could not be met, or the budget that bound the
  occupancy, which `occupancyLimiterName` already answers.
- **4.4** When a variant is refused, the route table's next row serves
  the work. This is what makes "every format runs on the host" a
  structural property rather than a promise: the refusal is what selects
  the fallback.
- **4.5** When geometry cannot help — a memory-bound kernel already at
  the roofline — the adaptor says so rather than searching.
  `LaunchPick.geometryWontHelp` is that answer and it already exists.

## 5. Adaptation: geometry and specialization constants

- **5.1** When a launch shape is derivable from the profile, it is
  derived and not frozen — the superseded draft's §2.4 inventory of
  frozen sites is the work list.
- **5.2** When a kernel parameter is a device quantity the body can be
  specialized on — wave width, tile extent, unroll — it is bound as a
  specialization constant at load rather than compiled in.
- **5.3** When two kernels differ ONLY by a device quantity, they become
  one kernel with that quantity specialized. MXFP4's coop and portable
  pair is the reference case.
- **5.4** When a kernel is specialized per device, the specialization is
  keyed and cached, so a second bind on the same device does not repeat
  it.

## 6. Bounded search, inside the adaptor

Search is the adaptor's third tier, not a separate system. The ordering
is the one the hardware-profile research recorded: **analytical first,
guidance second, search last and bounded** — an analytical model reaches
94.7% of exhaustive autotuning with tuning time reduced to zero
(tritonBLAS, findings §9.1), so search is what runs where the model
declines to answer.

- **6.1** When the profile marks a device unmodelable — and
  `LaunchPick.needsSweep` is already that signal — the adaptor verifies
  its choice empirically instead of trusting it.
- **6.2** When a search runs, it is BOUNDED — a stated budget of
  configurations and of time, never an open sweep.
- **6.3** When a search runs, its space contains the configuration the
  analytical model chose, so the search can never return something worse
  than where it started. hipBLASLt's documented failure mode is the
  reason this is a requirement: "only as good as the search space you
  hand it".
- **6.4** When a search produces a winner, it is recorded against the
  device AND the kernel set that was measured, so a rebuild invalidates
  it rather than silently serving a stale answer.
- **6.5** When a search would cost the caller live work, it does not run
  on the critical path uninvited.

## 7. What follows

- Ranking survivors by measured cost — the tile family, and what finally
  retires `Route.priority()`, which is a declared integer standing where
  a measurement belongs.
- Self-tuning between runs: the research plan's Q5, whose premise (one
  bandwidth-bound GEMM generalized to "no real workload") the ALU-bound
  k-quant decode kernels have already undercut.
- Fleet calibration — sharing a recorded winner across nodes rather than
  rediscovering it per process, named as the gap in the route-table
  spec's §7 and owned by nothing.
