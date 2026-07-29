# dap-stepping — spec

## 1 Definition

### 1.1 Purpose
Real source-level stepping — step-over, step-in, step-out — for the cajeta
debugger, over DAP and in the IDE. Closes the CP6e gap.

### 1.2 Problem
The DAP server answers `next`/`stepIn`/`stepOut` with "unsupported request",
and the plugin's `startStep*` overrides silently resume to the next
breakpoint. In a session with one breakpoint, clicking step runs the rest of
the program with no new line highlight — observed live on samples/tour
2026-07-20. All the substrate exists: statement safepoints carrying
`locId` (→ file/line) + `fiberId` + the frame-chain head, the
DebugController park/resume rendezvous, and frame walking for stacks and
locals.

### 1.3 Constraints
- Built on the existing safepoint/DebugController seam; no new codegen.
- Stepping is per-fiber: a pending step matches safepoints only on the fiber
  that was stopped when the step was requested.
- Line-granular from the IDE's point of view: safepoints mapping to the step
  origin's line are skipped, so a multi-statement line stops once.
- A breakpoint or exception stop reached while a step is pending wins and
  clears the step (standard debugger behavior).
- Step-in enters whatever the call enters, stdlib included — the mounted
  stdlib sources (ide-symbol-index 8.2.4) make those frames navigable.

### 1.4 Non-goals
- "Just my code" filtering (skip-stdlib step-in) — explicit follow-up.
- Cross-fiber stepping or stepping a fiber other than the stopped one.
- Instruction-level stepping, run-to-cursor, watchpoints.

## 2 Step semantics (controller)

### 2.1 Requirements
The DebugController gains a pending-step mode set at resume time: the verb,
the origin fiber, the origin frame depth (frame-chain length at the stop),
and the origin line. On each safepoint while a step is pending, on the
origin fiber only:
- step-in parks at the first safepoint whose line differs from the origin;
- step-over parks at the first such safepoint at depth ≤ origin depth;
- step-out parks at the first such safepoint at depth < origin depth.
Parking reports the stop with reason `step` and clears the pending step. A
breakpoint/exception park clears it too.

### 2.2 Use cases
- 2.2.1 As a developer stopped at Tour.main:52, when I step over, then the
  debugger stops at the next executable line of main — including when line
  52's statement calls into the stdlib (the stdlib safepoints are deeper
  and do not park).
- 2.2.2 As a developer stopped on the last statement of a method, when I
  step over, then the debugger stops at the caller's next line (the ≤ rule:
  the frame returned, depth got shallower).
- 2.2.3 As a developer stopped at a call, when I step in, then the debugger
  stops at the callee's first executable line — a stdlib callee included.
- 2.2.4 As a developer inside a callee, when I step out, then the debugger
  stops at the caller's next line after the call.
- 2.2.5 As a developer with a step pending, when another fiber hits an
  armed breakpoint first, then the debugger stops there with reason
  `breakpoint` and the pending step is cleared.
- 2.2.6 As a developer, when I step and the stepped fiber never reaches a
  qualifying safepoint (e.g. blocks in a native call), then the session
  stays running and pause/breakpoints remain usable — a step never wedges
  the controller.

## 3 DAP surface

### 3.1 Requirements
The server implements `next`, `stepIn`, and `stepOut`: validate the request
against the currently stopped fiber, arm the pending step, resume, and emit
`stopped` with `reason: "step"` when it parks. A step request while running
(no parked stop) fails with a message, not a crash.

### 3.2 Use cases
- 3.2.1 As a DAP client stopped at a breakpoint, when I send `next` with
  the stopped thread's id, then I receive a success response followed by a
  `stopped`/`reason:"step"` event on that thread.
- 3.2.2 As a DAP client, when I send `stepIn`/`stepOut` in the same shape,
  then the analogous stop arrives.
- 3.2.3 As a DAP client, when I send a step with no stop parked, then the
  response is `success: false` with a readable message and the session is
  unchanged.

## 4 IDE integration

### 4.1 Requirements
`CajetaDebugProcess.startStepOver/startStepInto/startStepOut` send the real
DAP requests; the resume placeholder is deleted. The IDE highlights the new
line on the `step` stop exactly as it does for breakpoint stops.

### 4.2 Use cases
- 4.2.1 As a developer in CLion stopped in Tour.main, when I press F8/F7/
  Shift-F8, then the execution line moves as §2's semantics dictate, with
  frames and variables refreshed at each stop.

## 5 Regression coverage

### 5.1 Requirements
DapServerSession tests in cajeta_debug_test cover: step-over to next line,
step-over across a call (stdlib safepoints don't park), step-over off a
method's last line (caller stop), step-in to callee first line, step-out to
caller, same-line multi-statement skip, step request while running fails
cleanly, and breakpoint-wins-over-pending-step.

### 5.2 Use cases
- 5.2.1 As CI (the by-hand debug-tests run), when stepping regresses, then a
  protocol-level test fails rather than the gap resurfacing live in CLion.
