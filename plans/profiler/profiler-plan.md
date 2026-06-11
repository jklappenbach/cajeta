# Profiler plan

Source: `plans/current-focus.md` → **Profiler** (Profiler interface in IntelliJ
Plugin · Report format · Broad Capture · Specific method timing).

> **Status at authoring:** design phase. Scope-based timing (`withTrace { … }`)
> is sketched in `cajeta-docs/CajetaTorch.md` (item 14, `torch.profiler`) but no
> runtime implementation exists. The IntelliJ plugin defers a profiler UI to a
> later phase (`ide-plugins/idea/Plan.md`). This plan covers the **runtime
> profiling subsystem**; the IDE interface is a *consumer* of it.

Two layers, kept distinct:
1. **Runtime profiler** (this plan, primary) — captures timing/allocation data
   from a running Cajeta program and emits a report.
2. **IntelliJ profiler interface** (tracked in `ide-plugins/idea/Plan.md` as a
   Phase ≥2 item) — visualizes the report. Listed here for coordination; built
   after the runtime layer produces a stable report format.

This is a **scaffold**; capture mechanism and report format are Julian's call.

## Goals (from focus list)

- [ ] **Broad Capture** — whole-program profile: where wall-time goes across
      methods, ideally with the fiber scheduler in mind (the runtime is
      fiber-based — per-fiber vs per-carrier-thread attribution matters; see D2).
- [ ] **Specific method timing** — targeted measurement of named methods /
      scopes (the `withTrace { … }` scope form from `CajetaTorch.md`), low
      overhead, opt-in per call site.
- [ ] **Report format** — a stable, documented, machine-readable artifact (the
      contract the IDE and any CLI report renderer consume).
- [ ] **Profiler interface in IntelliJ Plugin** — *(tracked in the IDE plan)*
      consumes the report format; flame/tree view, hot-path highlighting.

## Open decisions (need Julian)

- **D1. Capture mechanism for Broad Capture.** Sampling profiler (periodic
  stack snapshots — low overhead, statistical) vs instrumentation (compiler
  inserts enter/exit probes — exact counts, higher overhead) vs both behind a
  flag? Sampling fits "broad" better; instrumentation fits "specific method
  timing." Likely **both**, but confirm priority for v1.
- **D2. Fiber attribution.** The scheduler multiplexes fibers onto carrier
  threads (`plans/net/cajeta-net-plan.md` design recap). Does the profiler
  attribute time per **fiber** (logical, what the user reasons about) or per
  **carrier thread** (what the OS sees)? Per-fiber is more useful and harder —
  needs scheduler cooperation. This is the load-bearing decision.
- **D3. Report format.** Reuse an existing ecosystem format (Chrome
  `chrome://tracing` JSON, `pprof`, collapsed-stack/`flamegraph` folded text) so
  existing tooling works, vs a Cajeta-native format the IDE reads directly?
  Strong lean: **emit a standard format** (Chrome tracing or pprof) so the data
  is useful outside the IDE too. Confirm.
- **D4. Activation.** Compile-time flag (`--profile`), env var, an API
  (`Profiler.start()/stop()`), or annotation (`@Profile`)? The `withTrace { }`
  scope form implies an API/lexical surface for *specific* timing; *broad*
  capture is likely a launch flag.
- **D5. Allocation/memory profiling in scope for v1?** `CajetaTorch.md` mentions
  memory tracking alongside timing. Time-only first, memory later — confirm.

## Coordination

- The **report format (D3)** is the contract the IntelliJ interface depends on —
  settle it before the IDE plugin profiler work starts.
- **Specific method timing** overlaps the `torch.profiler` `withTrace` design in
  `cajeta-docs/CajetaTorch.md` — reconcile so there's one timing API, not two.
- Streamed/live results coordinate with `plans/compiler/curses-output-plan.md`
  (shared "structured results to a consumer" notion).

## First step

- [ ] Resolve D1–D3, then write the phased build plan (runtime capture → report
      emission → CLI render → IDE consumer), test-first per the house discipline.
