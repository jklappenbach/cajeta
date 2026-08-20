# cajeta-profiler — whole-run CPU + GPU profiling (spec)

> Status: **approved 2026-08-20**. Authored with the **design** skill.
> The actionable *how* lives in `agents/cajeta-profiler-plan.md`.
>
> The GPU half of §5–§7 and §11–§12 is grounded in a research pass across CUDA,
> ROCm/HIP and Vulkan, with direct measurement on the gfx1151 reference machine.
> Findings marked **[measured]** below were taken on real hardware; everything
> else is documentation-derived and flagged where it matters.
>
> Supersedes the design-phase scaffold at `agents/cajeta/profiler/profiler-plan.md`,
> whose open decisions D1–D5 are resolved here (§14).

---

## 1. Definition

### 1.1 Purpose
`cajeta-profiler` is a **whole-run profiler** for Cajeta programs. It captures
where a program's time goes — across host threads, fibers, and GPU device
queues — and emits a single merged timeline that a developer browses
interactively.

### 1.2 The problem it solves
Cajeta has no way to answer "where did the time go" for a running program. The
`bench/` and `samples/profile/` suites measure *named benchmarks* against
competitors; they say nothing about an arbitrary program. The debugger can stop
a program but not characterize it. For GPU work there is nothing at all: kernel
launches funnel through one runtime function and no timing is recorded, so a
developer cannot tell a slow kernel from a stalled host, or see that the Vulkan
backend serializes every dispatch.

### 1.3 Scope
1. **Whole-run CPU sampling** — no annotations, no per-call-site opt-in. A
   profiled run captures the entire program.
2. **Exact instrumentation** — an opt-in build mode adding call counts and exact
   enter/exit timing on top of sampling.
3. **Fiber-accurate attribution** — time attributed to the logical fiber, not the
   carrier thread.
4. **GPU device kernel timing** — real device-side start/end per kernel dispatch,
   correlated to the host call site that launched it, published through a seam
   with more than one consumer (§5.6).
5. **A merged trace** in Perfetto protobuf, with host threads, fibers, and GPU
   queues on one timeline in one clock domain.
6. **An interactive viewer** in the IntelliJ plugin.

### 1.4 Non-goals
1. **Hardware performance counters.** Occupancy, cache-hit rates, and
   instruction mix are out of scope. They require elevated privileges on both
   vendors, force kernel serialization or multi-pass replay, and perturb what
   they measure. Users wanting them are directed to `rocprofv3` and Nsight.
2. **Replacing vendor tools.** Deep single-kernel analysis remains rocprofv3 /
   Nsight / RGP territory. `cajeta-profiler` answers "where does my program spend
   time", not "why is this one kernel slow at the instruction level".
3. **Distributed or multi-process profiling.** One process per trace.
4. **Production always-on profiling.** Profiling is opt-in per run.
5. **A new trace format.** Perfetto is adopted, not invented.

### 1.5 Constraints
1. **The runtime already carries a logical call stack.** `--line-info` is on by
   default in every build flavor and maintains a per-thread array of
   `{type, method, file, line}` frames. Sampling reads it; no unwinding, no
   DWARF, no frame pointers, and it survives inlining because the probe inlines
   with the body.
2. **All GPU dispatch funnels through one function.** `__cajeta_xpu_launch_v3`
   is the single instrumentation point for every backend; `_v2` and the original
   `__cajeta_xpu_launch` are forwarding shims onto it
   (`runtime/native/cajeta_xpu_launch.c:985`). **Corrected 2026-08-20** — this
   read "`__cajeta_xpu_launch` → `_v2`", which was true when the spec was
   authored and is not now. Instrumenting `_v2` would silently miss every caller
   that reaches `_v3` directly. The file's own convention is that a new field
   arrives as a new version rather than by repurposing an argument, so the
   innermost version is the only durable seam.
3. **Vendor libraries are loaded dynamically.** Every GPU profiling dependency
   must be `dlopen`-ed and absent-tolerant.
4. **Profiling must not require privileges** for its baseline tier on any
   supported platform.
5. **Timestamps do not require serialization; counters do.** This is why §1.4.1
   excludes counters and why kernel timing can be concurrency-safe.

### 1.6 Naming
Three names are already taken in this codebase and must not be reused:
`--profile` / `@Profile` (DI profile selection), `samples/profile/` and
`specs/profile-spec.md` (the cross-language benchmark suite), and
`cajeta gpu-profile` (prints the device *capability* descriptor). This capability
is `cajeta-profiler`; its CLI verb is `cajeta profiler`; its build flag is
`--profiler`; its environment variable prefix is `CAJETA_PROFILER_`.

`CompilerFlags::profileCounters` exists, defaults on in `DebugRelease`, and is
consumed by nothing but the cache key. It is a reserved PGO name and is not
adopted here.

---

## 2. Whole-run CPU capture

Sampling reads the existing line-info shadow stack from a dedicated sampler
thread. Because the shadow stack is emitted by default, an ordinary already-built
binary can be profiled with no recompilation.

- **2.1** When a run is armed for profiling, every host thread and fiber is
  sampled for the whole run without any source annotation or per-call-site opt-in.
- **2.2** When a binary was built with default flags, it can be profiled with no
  rebuild.
- **2.3** When a sample is taken, the captured stack is the logical Cajeta call
  stack with type, method, file, and line — not a native backtrace.
- **2.4** When the program is compiled at `-O2` or `-O3` and functions are
  inlined, samples still attribute to the source-level call structure.
- **2.5** When a binary was built with `--line-info=off`, profiling fails loudly
  at arm time rather than silently producing an empty trace.
- **2.6** When the sample rate is configured, it is honored; when it is not, a
  documented default applies.
- **2.7** When a thread is created after profiling starts, it is sampled from
  creation.
- **2.8** When the shadow stack is deeper than its fixed capacity, the sample
  records that it was truncated rather than reporting a shallower stack as
  complete.
- **2.9** When profiling is not armed, a profiled-capable binary pays no cost
  beyond the line-info instrumentation it already carries.

## 3. Exact instrumentation

An opt-in build mode adds exact call counts and enter/exit timing. It is a
separate tier from sampling, not a replacement: sampling answers "where does wall
time go", instrumentation answers "how many times, and how long exactly".

**Why this is built rather than delegated.** Callgrind already produces exact,
deterministic call counts and needs no compiler work — and once fiber stacks are
registered (see [`valgrind-interop`](valgrind-interop-spec.md) §3) it would work
on Cajeta programs. It is a genuinely useful tool for regression comparison and is
recommended for that. It cannot serve this tier for four reasons, each
independently disqualifying:

1. **Roughly 50× slowdown.** Unusable for the whole-run profiling of §2.
2. **Simulated, not measured.** Callgrind models a cache and counts instructions;
   it does not observe the real machine, so its numbers are repeatable but are not
   this hardware's behavior.
3. **No GPU visibility.** Work dispatched to a device is invisible, so it cannot
   participate in the merged timeline that is the point of §5 and §7.
4. **Cannot run on device targets or under the JIT** the way in-process
   instrumentation can.

The requirement is therefore exact counts *at low overhead, in-process, merged
with the GPU timeline, and on every target the language supports*.

- **3.1** When a program is built with `--profiler=instrument`, every Cajeta
  method records an exact entry count and inclusive time.
- **3.2** When a program is built without that flag, no instrumentation probes are
  emitted and there is no residual cost.
- **3.3** When instrumentation is enabled, the probes are recognized as
  instrumentation by the existing trivial-drop elision analysis, so enabling
  profiling does not defeat unrelated optimizations.
- **3.4** When both sampling and instrumentation are active, both appear in one
  trace and are distinguishable by source.
- **3.5** When an instrumented build runs, the overhead is reported in the trace
  so the developer can judge how much the measurement distorted the program.
- **3.6** When a method is inlined, its instrumentation still records the call.
- **3.7** When the flag set changes, the compile cache key changes, so an
  instrumented and non-instrumented build never alias.

## 4. Fiber attribution

The runtime multiplexes stackful fibers onto carrier threads. The existing
shadow stack is per-carrier-thread, so a fiber that yields leaves stale entries —
a correctness defect today, and fatal to a profiler.

- **4.1** When a fiber yields and another resumes on the same carrier thread, each
  fiber's samples attribute to that fiber, not to the carrier.
- **4.2** When a fiber migrates between carrier threads, its stack remains intact
  and correctly attributed.
- **4.3** When a trace is viewed, fibers appear as their own timeline tracks,
  distinct from carrier threads.
- **4.4** When a stack trace is captured from within a fiber, it resolves
  correctly — the same fix that enables 4.1 removes the existing staleness defect.
- **4.5** When the main program thread (which is not a fiber) is sampled, it is
  attributed correctly alongside fibers.

## 5. GPU device kernel timing

Each backend needs a different mechanism; in every case the obvious one is wrong.
The seam is a backend-neutral event record, not a shared vendor API — the vendor
APIs are structurally incompatible and cannot be unified below that level.

That record has **more than one consumer**. The trace writer (§7) is the first and
this spec's own client. The second is the XPU scheduler's feedback loop
([`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md) §3, §8), which classifies
each kernel by bottleneck and then corrects that classification from achieved
versus predicted latency — a signal nothing else in the runtime produces per
launch. §5.6 states what serving both costs: publication to a sink rather than a
direct call into the writer. It is stated here, before the writer exists, because
splitting a wired-in writer back out later is a rewrite of the collection path in
every backend.

### 5.1 Common requirements
- **5.1.1** When a kernel is dispatched on any backend, a record is emitted
  carrying a launch id, device start and end times, the launching host thread, and
  the kernel's identity and geometry.
- **5.1.2** When a kernel executes, it is correlated to the host call site that
  launched it.
- **5.1.3** When device timing is active, kernel concurrency is preserved — the
  profiler does not serialize the program to measure it.
- **5.1.4** When a backend cannot provide device timing, the run degrades to
  host-side submit-to-complete timing and the trace states which tier produced
  each measurement.
- **5.1.5** When timing is enabled, the per-launch overhead is bounded and
  reported.
- **5.1.6** When a record is complete, it is published to a **record sink** rather
  than written directly to the trace writer; the writer is one sink implementation
  among others (§5.6).
- **5.1.7** When a record is published, it is already in the host clock domain
  (§6), so every sink sees correlated times and no consumer repeats the
  correlation — or gets it differently wrong.

### 5.2 ROCm / HIP
rocprofiler-sdk buffered dispatch tracing is both the cheapest and the most
accurate option — the reverse of the usual tradeoff. **[measured]** on gfx1151:
+78% launch overhead and exact durations, against +247% and a 7.4× overstatement
for `hipEventRecord` bracketing, which also *perturbs the kernel it measures*
because its markers carry system-scope fences.

- **5.2.1** When the HIP backend is profiled, device timing comes from
  rocprofiler-sdk dispatch records, not from HIP events.
- **5.2.2** When rocprofiler-sdk is not installed, device timing degrades rather
  than failing the run.
- **5.2.3** When profiling is armed, rocprofiler-sdk is configured before the
  first HIP call, since configuring later yields zero records while returning
  success on every call.
- **5.2.4** When the SDK is configured but no records arrive after a threshold
  number of dispatches, device timing is disabled and the failure is reported —
  this failure mode is otherwise invisible.
- **5.2.5** When a launch is recorded, the profiler's own launch id is carried
  through as an external correlation id.

### 5.3 CPU emulation backend
- **5.3.1** When kernels run on the CPU emulation backend, they appear as a device
  lane on the timeline with the same record shape as a real device.
- **5.3.2** When no GPU is present, the GPU timing path is still exercisable —
  giving a hardware-free test path for the whole pipeline.

### 5.4 NVIDIA / CUDA
- **5.4.1** When the CUDA backend is profiled, device timing comes from the CUPTI
  Activity API using `CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL`, never
  `CUPTI_ACTIVITY_KIND_KERNEL`, which serializes all kernel execution.
- **5.4.2** When CUPTI is present, it is found by explicit path search, because it
  ships with the CUDA Toolkit and is deliberately not on the loader path.
- **5.4.3** When another CUPTI subscriber holds the process (Nsight, `nsys`,
  `cuda-gdb`), the profiler degrades to a no-op and reports it rather than
  aborting.
- **5.4.4** When the driver's profiling permission gate is active, baseline kernel
  timing still works for an unprivileged user. *This is inferred, not
  documentation-cited, and §12.4 requires it be proven before v1 depends on it.*
- **5.4.5** When activity records are read, only stable prefix fields are used, so
  a toolkit shipping a newer record version does not break parsing.

### 5.5 Vulkan
- **5.5.1** When the Vulkan backend is profiled, device timing comes from
  timestamp query pools.
- **5.5.2** When a queue family is selected for compute, a family with non-zero
  `timestampValidBits` is preferred; **[measured]** three of five families on the
  reference device report zero, where timestamps are silently meaningless.
- **5.5.3** When a dispatch is bracketed, an explicit pipeline barrier precedes the
  closing timestamp — without it, some drivers latch the timestamp before the
  kernel completes, yielding durations wrong by four to five orders of magnitude,
  and `ALL_COMMANDS` does not prevent it.
- **5.5.4** When query results are read, availability is checked rather than the
  value, because an unavailable result leaves the destination buffer untouched.
- **5.5.5** When a query slot is reused, it is reset before reuse; an unreset slot
  reports as available carrying the previous use's value.
- **5.5.6** When timestamps wrap at the device's valid-bit width, the wrap is
  handled without undefined behavior at 64 bits.
- **5.5.7** When device timestamps are non-monotonic — as happens on AMD APUs,
  which reset the timestamp register on low-power entry — the condition is
  detected and the affected spans are flagged rather than rendered as fact.

### 5.6 Record consumers

The trace writer is a sink, not the seam. The second known consumer needs the same
per-launch records **in-process and while the program runs**, not as a file after
it exits.

- **5.6.1** When a consumer registers a sink, it receives dispatch records as they
  are collected, with no trace file written and no change to the capture or
  dispatch path.
- **5.6.2** When more than one sink is registered, each receives every record, and
  no sink can observe or mutate what another receives.
- **5.6.3** When a sink is armed, it is armed independently of trace output. A live
  consumer does not imply `.pftrace` emission, and emitting a trace does not imply
  a live consumer.
- **5.6.4** When a sink is slow or fails, it cannot stall the dispatch path or the
  collection thread. Delivery is bounded; records that cannot be delivered are
  dropped, and the drop is counted and reported — never silently absorbed, and
  never converted into backpressure on the program under test.
- **5.6.5** When a sink faults, the profiler isolates the failure, disables that
  sink and reports it; the remaining sinks and the run continue.
- **5.6.6** When records are delivered live, they carry the same tier and
  confidence marking §5.1.4 and §7.8 require, so a consumer can tell a real device
  measurement from a degraded host-side one and weight it accordingly. A consumer
  that acts on a degraded measurement as though it were exact is a consumer bug,
  but the seam must give it the means not to.
- **5.6.7** When a consumer wants only some backends or kernels, filtering is the
  consumer's business. The seam delivers every record and does not grow a query
  language.

This spec defines the seam and the record, **not scheduling policy** — what a
scheduler does with achieved-versus-predicted latency belongs to
`xpu-kernel-scheduling` §3 and §8. §1.4.1's exclusion of hardware counters is
unchanged by this: the live sink carries timing, identity and geometry, not SM or
bandwidth counters, and a consumer wanting those degrades to latency-only signals,
which that spec's §12.4 already requires of it.

## 6. Clock correlation

All lanes must land on one host timeline. Each backend reaches it differently,
and the natural default on each is wrong in a way that produces confident,
plausible, badly incorrect numbers.

- **6.1** When any device timestamp is emitted, it is expressed in the same host
  clock domain as CPU samples.
- **6.2** When the CUDA backend is used, CUPTI is given the profiler's own clock
  reader so records arrive already in the host domain with no conversion step.
- **6.3** When the ROCm backend is used, the device domain's offset from the host
  clock is measured rather than assumed. **[measured]** the domain originates as
  `CLOCK_BOOTTIME`; it tracks `CLOCK_MONOTONIC` to under a microsecond but
  diverges across a suspend.
- **6.4** When a trace spans a system suspend, the offset is resampled and the
  trace is flagged rather than silently sheared.
- **6.5** When the Vulkan backend is used, the `CLOCK_MONOTONIC` time domain is
  requested explicitly. **[measured]** the driver's own preference order puts
  `CLOCK_MONOTONIC_RAW` first, which sits 5.68 seconds away from the domain ROCm
  uses — accepting each backend's default would put the two lanes seconds apart
  with nothing reporting an error.
- **6.6** When a Vulkan trace runs longer than a few seconds, calibration is
  refreshed. **[measured]** the reference device drifts −15 ppm, about 54 ms per
  hour, and its raw device ticks sit 104.6 s from the host clock, so a single
  calibration is insufficient and `timestampPeriod` alone is useless.
- **6.7** When calibration samples are taken, poor-quality samples are rejected,
  with a bounded retry so a device in an unfavorable power state cannot hang the
  profiler.
- **6.8** When the host platform is Windows, the host clock is the platform's
  high-resolution counter rather than a POSIX clock, and correlation works
  identically.

## 7. Trace format

Perfetto protobuf is adopted. Chrome Trace Event JSON cannot represent this data:
its clock-sync events are not parsed by any current consumer, so every timestamp
must be pre-converted into one domain — precisely what §6 establishes we cannot
correctly do at emit time.

- **7.1** When a run completes, a `.pftrace` file is written that opens in
  `ui.perfetto.dev` with no conversion.
- **7.2** When GPU work is traced, each device, context, and queue is a real
  track in a hierarchy, not a synthetic thread.
- **7.3** When a kernel is traced, the flow from its host launch site to its device
  execution is expressed so the causal link is navigable.
- **7.4** When kernel names repeat, they are interned and emitted once.
- **7.5** When a device clock domain is used, clock snapshots are emitted at each
  recalibration so the mapping is reproducible from the trace itself.
- **7.6** When the profiler is killed mid-run, the partial trace is still a valid,
  readable file.
- **7.7** When the trace is written, the emitter depends only on the stable
  `TrackEvent` / `TrackDescriptor` schema; any GPU-specific packets are optional
  additions, because those are explicitly outside Perfetto's stability guarantee.
- **7.8** When a trace is produced, it records the driver identity, active layers,
  calibration quality, and which timing tier produced each measurement.

## 8. Interactive browsing

The IntelliJ plugin is the primary client. It is Kotlin, so it reads the trace
directly rather than through a Cajeta library; the format is the contract.

- **8.1** When a profiled run finishes, its trace can be opened in a tool window
  from the IDE.
- **8.2** When a trace is open, a flame graph shows where wall time went, and
  selecting a frame navigates to the source location.
- **8.3** When a trace contains GPU work, a timeline view shows host threads,
  fibers, and device queues on one time axis.
- **8.4** When a kernel is selected, its launching call site is reachable in one
  action.
- **8.5** When a trace contains exact instrumentation data, call counts are shown
  alongside sampled time.
- **8.6** When measurements are flagged as low-confidence or from a degraded tier,
  the UI shows that rather than presenting them as equivalent.
- **8.7** When per-item intervals are not additive — as with Vulkan queue
  occupancy — they are presented as relative indicators and never summed into a
  cost breakdown.
- **8.8** When a developer prefers an external viewer, the same file opens in
  Perfetto with no export step.

## 9. Activation and configuration

- **9.1** When `CAJETA_PROFILER` is set, the run is profiled; when it is unset,
  nothing is loaded and nothing is armed.
- **9.2** When profiling is armed by environment variable alone, no rebuild is
  required for the sampling tier.
- **9.3** When exact instrumentation is wanted, it is selected at build time,
  because it changes codegen.
- **9.4** When the output path is configured, the trace is written there; when it
  is not, a documented default location is used.
- **9.5** When the build tool runs a task, profiling can be requested through it
  without hand-editing environment variables.
- **9.6** When profiling is armed for a program that will use the GPU, arming
  happens early enough to satisfy every backend's initialization ordering
  requirement.

## 10. Capability detection and degradation

- **10.1** When a vendor library is absent, the affected tier is unavailable and
  the rest of the profiler still works.
- **10.2** When a device node exists but is not accessible, the failure is
  distinguished from the device being absent, and the message names the fix.
- **10.3** When a facility fails to initialize partway through setup, everything
  already enabled is unwound and profiling for that backend is disabled, rather
  than leaving a half-configured state.
- **10.4** When a tier is unavailable, the profiler degrades through a documented
  ladder rather than failing the run.
- **10.5** When running under WSL, tiers unsupported there are demoted at
  detection time rather than failing confusingly at use.
- **10.6** When any capability is degraded, the trace records which tier was used
  so a developer never mistakes a degraded measurement for a full one.

## 11. Integrity and self-verification

Nearly every failure mode found in the research returns success and plausible
numbers. Assertions are therefore a feature of this capability, not an
afterthought.

- **11.1** When device timing is enabled, the profiler verifies at startup that
  end exceeds start, that durations are within a sane bound, and that consecutive
  dispatches produce different timestamps; a tier failing this is demoted.
- **11.2** When a backend produces no records after a threshold, that backend's
  timing is disabled and reported.
- **11.3** When device timestamps are non-monotonic or implausible, affected spans
  are flagged in the trace.
- **11.4** When a driver reports an implausible timestamp period, it is rejected
  rather than used.
- **11.5** When records are dropped by a vendor buffer, the count is recorded in
  the trace rather than rendered as a gap.
- **11.6** When the profiler cannot establish a trustworthy clock correlation, it
  says so instead of emitting a plausible timeline.

## 12. Platform and driver support

- **12.1** When running on Linux with AMD hardware, the baseline and device tiers
  work unprivileged.
- **12.2** When hardware counters would be needed, they are out of scope (§1.4.1),
  so no privileged setup is ever required.
- **12.3** When running on Windows, sampling and CUDA device timing work, using the
  platform's native clock and library conventions.
- **12.4** When the NVIDIA permission gate's scope is verified on real hardware,
  §5.4.4 is confirmed or the design is revised. This is the one load-bearing
  inference in the research and must be settled before v1 depends on it.
- **12.5** When running under WSL, CPU sampling and CUDA kernel timing work.
- **12.6** When a supported platform's toolchain is missing a required component,
  detection reports precisely which one.

## 13. Performance budgets

- **13.1** When sampling is armed, added overhead stays within a stated budget for
  a program under test, and the budget is documented.
- **13.2** When device timing is armed, per-launch overhead is stated per backend
  and measured, not estimated.
- **13.3** When exact instrumentation is armed, its cost is expected to be
  substantially higher than sampling and is measured and published so developers
  choose knowingly.
- **13.4** When profiling is not armed, overhead is zero beyond what the binary
  already carried.
- **13.5** When any budget is exceeded on a supported configuration, that is a
  defect.
- **13.6** When a live sink is registered, its delivery cost is bounded and stated
  separately from trace writing, because its consumer sits on the program's
  critical path where the trace writer does not.

---

## 14. Resolved decisions

The scaffold plan's open decisions, closed 2026-08-17 with the research and the
developer's direction.

- **14.1 Capture mechanism (was D1).** Both. Sampling is the default tier and
  needs no codegen changes; exact instrumentation is an opt-in build mode (§3).
- **14.2 Fiber attribution (was D2).** Per-fiber, not per-carrier-thread (§4).
  This requires fixing an existing correctness defect in the shadow stack, which
  is a prerequisite for the profiler and a bug fix in its own right.
- **14.3 Report format (was D3).** Perfetto protobuf, emitted directly without the
  SDK (§7). Settled by the clock-domain limitation of Chrome JSON, not preference.
- **14.4 Activation (was D4).** Environment variable for sampling, build flag for
  instrumentation (§9). `--profile` and `@Profile` were unavailable (§1.6).
- **14.5 Memory profiling (was D5).** Out of scope for v1, but the reason has
  weakened and this should be revisited before v2. The allocator has a single
  chokepoint that makes it cheap; the objection was that the frame bump arena
  bypasses it, leaving the picture incomplete.
  [`memory-tooling`](memory-tooling-spec.md) Unit 3 annotates the arena regardless,
  and those annotation points are exactly the accounting points — so the
  prerequisite is being built for other reasons.
  Worth noting Cajeta can do this better than an external tool: a sanitizer's
  allocation stacks need a native unwind and yield native frames, whereas
  capturing a Cajeta stack at an allocation site is a memcpy of the shadow stack —
  no unwinding, real Cajeta frames, and it works with no sanitizer build and on
  device targets.
- **14.6 Interactive client.** IntelliJ tool window in v1, reading `.pftrace`
  directly in Kotlin. Perfetto's own UI remains available for the same file.
- **14.7 Backend order.** AMD and CPU first — both fully measured on hardware at
  hand — then NVIDIA once §12.4 is settled, then Vulkan once its device-creation
  and queue-family prerequisites are fixed.
- **14.8 Hardware counters.** Excluded (§1.4.1).
- **14.9 The record seam is dual-consumer (decided 2026-08-20).** The dispatch
  record publishes to sinks; the Perfetto writer is one of them (§5.6). The second
  known consumer is the XPU scheduler's feedback loop
  ([`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md) §3, §8), which needs
  per-launch achieved latency live and in-process. That spec assumed these signals
  would come from `xpu-device-profile`'s counter tier, which is opt-in, in-memory
  and built to calibrate a launch-config picker — it does not produce a per-launch
  stream. This seam does. A sink interface costs one indirection now; discovering
  the second consumer after the writer is wired in costs a rewrite of the
  collection path in every backend, which is why the decision is taken before the
  seam is built rather than when the scheduler needs it. **v1 ships the seam and
  the writer sink**; the scheduler's sink ships with the scheduler, not here.

## 15. Open questions

- **15.1** Whether the sampling tier should also capture on Vulkan's `vkQueueWaitIdle`
  stalls as an explicit "host blocked on GPU" span, or leave that to be inferred
  from the timeline. Cheap either way; affects only presentation.
- **15.2** Whether the IntelliJ viewer ships in the same release as the runtime
  capture, or trails it by one. The trace format is the contract either way.
- **15.3** Whether `--profiler=instrument` should imply a specific optimization
  level, given that instrumented builds at `-O3` measure a different program than
  the one shipped.
- **15.4** Whether a live sink is handed each record as it is collected or drains
  batches, which trades a consumer's reaction latency against per-record delivery
  cost. It does not change the seam, and the answer is likely per-sink rather than
  global; a scheduler correcting a classification over the first few executions may
  well tolerate batches that a deadline-class policy would not.
