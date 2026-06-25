# Cajeta Embedded / Remote Targets & Telemetry — IDE Plugin — Spec

> Status: **DRAFT for review** (design skill). Specs the IntelliJ-plugin
> capabilities for **executing, debugging, and observing Cajeta on embedded /
> remote devices** — Raspberry Pi / Jetson, RTOS boards, bare-metal MCUs, Arduino
> — for robotics and on-device AI. The **memory viewers** these depend on are in
> [`memory-viewer-spec.md`](memory-viewer-spec.md); the **build/JSONL** surfaces
> they extend are in [`buildtool-widget-spec.md`](buildtool-widget-spec.md).
>
> Grounds in [`docs/Embedded.md`](../Embedded.md) (target tiers T1–T4, modes
> E1–E10, per-tier size targets), [`docs/Debugging.md`](../Debugging.md) (DAP),
> and [`docs/BuildTool.md`](../BuildTool.md) (`${target}` triple, `cajeta
> toolchain`). Plan at `agents/embedded-targets-plan.md` once approved.

---

## 1. Definition

### 1.1 Purpose
Make the plugin a first-class environment for embedded/robotics/AI development:
register devices, cross-compile and deploy to them, run and **debug on-device**,
and **observe** live telemetry, task scheduling, and resource footprint — across
the four `Embedded.md` tiers (T1 IoT-Linux works today; T2 RTOS, T3 bare-metal,
T4 deeply-embedded are roadmap).

### 1.2 Scope (v1)
- A **Devices** tool window + target registry (SBC over SSH, MCU over serial/
  USB/probe), with arch/tier/status.
- **Cross-compilation & toolchain** selection per target (`${target}` triple +
  `cajeta toolchain`), surfaced in the build widget.
- **Deployment & flashing** — push artifacts to Linux SBCs (scp/rsync), flash
  firmware to MCUs (OpenOCD/probe-rs/avrdude), with deploy configs.
- **On-device execution** + **remote/probe debugging** (DAP over TCP for T1/T2;
  probe/gdbserver bridge for T3/T4).
- **Serial monitor / SSH console**.
- **Telemetry**: live JSONL telemetry stream + **real-time charts**, and a
  **scheduling/fiber-task timeline** (jitter/deadlines).
- **Footprint** integration (the memory-viewer Viewer D) per target tier.
- **AI/model deployment & accelerator telemetry** (Jetson / `cradle`).
- **Mode/tier awareness** — surface `--mode=embedded-linux`/heapless/etc. as
  build profiles + lint for tier-unavailable features.

### 1.3 Problem
Today the plugin builds/debugs only on the host (the debugger is JIT-in-process,
local). Embedded work requires cross-compiling to a device, getting the artifact
there, debugging it *on the device*, and watching real-time behavior (control-
loop timing, sensor telemetry, RAM headroom) — none of which the plugin does.

### 1.4 Constraints & dependencies
- **The remote/AOT enabler (long pole).** On-device debugging needs the **AOT +
  DWARF + remote-attach** path (`Plan.md` Part A): a `cajeta dap` (or probe/
  gdbserver bridge) running *on the device*, driven over `cajeta dap --port`
  (TCP exists; the in-process JIT model cannot attach to a separate process).
  Most of §5–§6 and on-device memory viewers depend on this; it is a cross-repo
  prerequisite (§15), called out, not solved here.
- **Build tool.** `${target}` build-target triple, `cajeta toolchain`
  (provisioning), build flavors/modes already exist (`docs/BuildTool.md`,
  `Embedded.md` E1). This spec drives them from the IDE.
- **Reuse.** Telemetry/console reuse the **JSONL render engine**
  (`buildtool-widget-spec.md` §7); the scheduling timeline reuses the **fibers
  view** (CP6f-2); footprint reuses **Viewer D** (`memory-viewer-spec.md` §5);
  run/debug reuse the existing `debugger/` + build-widget run configs.
- **Cores plain JVM** — device registry, deploy/flash command construction,
  telemetry/scheduling models — unit-tested without a platform fixture.

### 1.5 Non-goals (v1)
- Implementing the runtime embedded tiers (E1–E10 are compiler/runtime work).
- A vendor board-support ecosystem / driver library.
- Fleet orchestration beyond single-device deploy (OTA-to-many is a follow-up).
- Replacing existing host run/debug — this extends it to remote/embedded.

---

## 2. Devices tool window & target registry

### 2.1 Requirements
A **Devices** tool window listing registered targets: name, transport (SSH /
serial / USB / probe), arch + `Embedded.md` **tier**, online/offline status,
build target-triple + toolchain. Add/edit/remove targets; auto-detect serial/USB
where possible. The registry persists per project.

### 2.2 Use cases
- **2.2.1** As a developer, when I add an SSH device (host/user/key), then it
  appears with reachability status and an editable target-triple/toolchain.
- **2.2.2** As a developer, when I plug in a serial/USB board, then it is
  offered for one-click registration.
- **2.2.3** As a developer, when a device goes offline, then its status reflects
  it and dependent run configs surface the unavailability (not a silent hang).

---

## 3. Cross-compilation & toolchain selection

### 3.1 Requirements
Per target, select the build **target-triple** and **toolchain** (via `cajeta
toolchain`), surfaced as a selector in the build-tool widget so a task builds for
the chosen device. Missing toolchains offer install via `cajeta toolchain
install`.

### 3.2 Use cases
- **3.2.1** As a developer, when I pick an `aarch64` Pi target and run `build`,
  then `cajeta build` runs with that target-triple/toolchain.
- **3.2.2** As a developer, when the toolchain isn't installed, then I'm offered
  `cajeta toolchain install`, and after it the build proceeds.

---

## 4. Deployment & flashing

### 4.1 Requirements
A **deploy** step per target: copy the built artifact to Linux SBCs (scp/rsync to
a configured path) or **flash** firmware to MCUs (OpenOCD / probe-rs / avrdude
per descriptor). Deploy config (target + remote path / flash tool + args)
saveable and chainable after a build.

### 4.2 Use cases
- **4.2.1** As a developer, when I "Build & Deploy" to a Pi, then the artifact
  builds and is copied to the device path, reporting success/failure.
- **4.2.2** As a developer, when I flash an MCU, then the configured flash tool
  runs and its output streams to the console, with clear pass/fail.
- **4.2.3** As a developer, when deploy/flash fails (unreachable / tool error),
  then the failure surfaces non-fatally with the tool's output.

---

## 5. On-device execution & remote/probe debugging

### 5.1 Requirements
Run a deployed artifact on the device (SSH exec / monitor reset) with output
streamed to a console. **Debug on-device**: a **Remote Attach** run-config →
on-device `cajeta dap` over TCP (T1/T2); a **probe bridge** (OpenOCD/J-Link/
probe-rs via DAP) for bare-metal (T3/T4). Breakpoints/stepping/variables reuse
the existing XDebugger plumbing; the memory viewers (`memory-viewer-spec.md`)
attach to the same session.

### 5.2 Use cases
- **5.2.1** As a developer, when I run on a Pi, then its stdout streams to a
  console with stop control.
- **5.2.2** As a developer, when I Debug-attach to an on-device `cajeta dap`,
  then my breakpoints bind and the debugger drives the remote program.
- **5.2.3** As an embedded developer, when I debug a Cortex-M via a probe bridge,
  then I hit breakpoints and inspect registers/memory (Viewers A/B).
- **5.2.4** As a developer, when the device link drops mid-session, then the IDE
  reports it and ends the session cleanly (no hang).

---

## 6. Serial monitor / device console

### 6.1 Requirements
A serial monitor (configurable baud/port) and an SSH console; stream device
stdout/telemetry; send input/commands; recognize JSONL lines and offer the
structured view (reusing the JSONL engine).

### 6.2 Use cases
- **6.2.1** As an Arduino developer, when I open the serial monitor, then I see
  device output live and can send a line.
- **6.2.2** As a developer, when device output is JSONL, then I can switch the
  monitor to the structured telemetry view (§7).

---

## 7. Telemetry stream & real-time charts

### 7.1 Requirements
Ingest a device **telemetry stream** (JSONL over serial/TCP) and render: the
structured JSONL view (reused engine) **plus real-time charts** — time-series
plots of numeric fields (sensor values, control-loop period/**jitter**, CPU/RAM),
configurable per field, with pause/zoom and a rolling window. A documented
telemetry record convention (§15) keeps device and IDE in sync.

### 7.2 Use cases
- **7.2.1** As a robotics developer, when my device emits telemetry, then I plot
  a sensor field live as a time series.
- **7.2.2** As a developer tuning a control loop, when I plot loop-period, then I
  see jitter/outliers against the target period.
- **7.2.3** As a developer, when I pause/zoom a chart, then the stream keeps
  buffering and resumes without loss.

---

## 8. Scheduling / fiber-task timeline

### 8.1 Requirements
A timeline of fiber/task scheduling over time (reusing the CP6f-2 fibers model):
per-fiber run/blocked spans, carrier mapping, and — where telemetry provides
deadlines — deadline-miss/jitter marking. For RTOS targets, fibers correspond to
cooperative tasks on a carrier (`Embedded.md`).

### 8.2 Use cases
- **8.2.1** As a developer, when I record a run, then I see each fiber/task's
  run/blocked spans on a timeline.
- **8.2.2** As a robotics developer, when a task misses its deadline, then the
  timeline marks the miss with its jitter.

---

## 9. Footprint per target tier

### 9.1 Requirements
The **Flash/RAM footprint viewer** (`memory-viewer-spec.md` §5, Viewer D) — which
lives in the **debug-view Memory panel**, not the build widget — is parameterized
by the **selected target's tier budget** (`Embedded.md` per-tier flash/RAM): its
static post-build bars + treemap, and its runtime RAM overlay during an on-device
session. The build widget at most offers a "Show footprint" action that opens
that debug-view panel for the selected target.

### 9.2 Use cases
- **9.2.1** As a developer, when I build for a T3 board, then the footprint view
  shows flash/RAM vs that tier's envelope and warns if over.
- **9.2.2** As a developer, when I debug on-device, then the runtime RAM overlay
  shows actual usage vs the device limit (per Viewer D).

---

## 10. AI/model deployment & accelerator telemetry

### 10.1 Requirements
For Jetson-class AI targets (`cradle`), a deploy+run flow for on-device inference
and accelerator telemetry (GPU/XPU utilization, memory, inference latency)
plotted via §7, tying into the núcleo / CajetaTorch stack where present.

### 10.2 Use cases
- **10.2.1** As a developer, when I deploy a model to a Jetson, then I run
  on-device inference and stream latency/utilization telemetry into charts.
- **10.2.2** As a developer, when GPU memory nears the limit, then the telemetry
  chart/threshold surfaces it.

---

## 11. Mode/tier awareness & lint

### 11.1 Requirements
Surface embedded **modes** (`--mode=embedded-linux`, heapless, etc.; `Embedded.md`
E1/E10) as build profiles selectable per target, and lint/flag source using
features unavailable in the selected tier/mode (e.g. heap allocation under
heapless, exceptions where excluded) using the existing lint surface.

### 11.2 Use cases
- **11.2.1** As a developer, when I select heapless mode and use `heap T(...)`,
  then the editor flags it as unavailable in that mode.
- **11.2.2** As a developer, when I pick an embedded mode, then builds for that
  target use it.

---

## 12. Settings

### 12.1 Requirements
Device registry persistence; default deploy/flash tools + args; telemetry stream
config (transport, record convention); chart defaults; per-target toolchain/mode.
Persisted via `CajetaSettings` / project-level storage.

### 12.2 Use cases
- **12.2.1** As a developer, when I configure a flash tool once, then it's reused
  for that target's flash actions.

---

## 13. Non-functional requirements

- **13.1 Performance.** Telemetry/console streaming and charts are off-EDT and
  windowed (rolling buffers); deploy/build/debug never block the EDT.
- **13.2 Cross-platform / cross-target.** Host OS Linux/macOS/Windows; targets
  across tiers T1–T4 via the remote enabler + probe bridges.
- **13.3 Graceful degradation.** Offline device, missing toolchain, failed
  deploy/flash, dropped link, absent telemetry, or the remote enabler not yet
  present degrade to a clear non-fatal state — never a hang or error dialog.
- **13.4 Testability.** Device registry, deploy/flash command construction,
  telemetry parsing/series model, and scheduling model are plain-JVM cores with
  unit tests; transport/integration tested against fixtures/loopback, skipping
  when no device/host is present.
- **13.5 Reuse / single source of truth.** Telemetry uses the JSONL engine;
  scheduling uses the fibers model; footprint uses Viewer D; on-device memory
  uses the memory-viewer DAP path — no parallel implementations.

---

## 14. Relationship to the other specs

- **`buildtool-widget-spec.md`** — adds the per-target build/run/deploy controls
  and supplies the JSONL engine telemetry reuses.
- **`memory-viewer-spec.md`** — its Viewers A/B (raw/register) and D (footprint)
  are the on-device memory/footprint surfaces this spec attaches; the **remote/
  AOT enabler (§15.1)** is shared between the two.
- **`cja-source-view-spec.md`** — independent, but on-device step-into benefits
  from the same source-resolution.

---

## 15. Cross-repo prerequisites

- **15.1 AOT + DWARF + remote-attach** debug path (`Plan.md` Part A) — the long
  pole for on-device debugging and on-device memory viewers (`cajeta dap --port`
  driving an on-device/AOT target, or a probe/gdbserver bridge).
- **15.2 Telemetry record convention** — a documented JSONL telemetry schema
  (ts/level/metric/value/…) devices emit and the IDE charts.
- **15.3 Per-target toolchains** in `cajeta toolchain` for the supported triples
  (ARM/AArch64/Cortex-M/AVR).
- **15.4** Build-tool size report (`memory-viewer-spec.md` §7.5) and stack
  high-water probe (§7.6) for footprint at a tier.
