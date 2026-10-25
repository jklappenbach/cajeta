# dap-attach-missing — the debug adapter documents `attach` and implements only `launch` (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 15 on v0.46.3.

## 1. Definition
`docs/guide/05-debugging.md` lists a DAP `attach` request; `DapServer` handles
only `launch`. `toolchain/debugger-attach` therefore measures launch-to-first-
breakpoint for Cajeta against real attaches for jdb, gdb, delve and debugpy,
and says so. From the same unit: no command runs a `.cja` archive through
the JIT (`jit-run` and the DAP launch take a source root, and an uber `.cja`
has no runner), so the start-up rows use `jit-run` on a source root.

## 2. Use cases
- **2.1** As a developer, when I send `attach` with a process id, then the
  adapter attaches to a running Cajeta process built with debug info and
  breakpoints work; or the guide stops listing it until it does.
- **2.2** As a developer, when I have a `.cja`, then `cajeta run <archive>`
  (or `jit-run <archive>`) executes it through the JIT with the machine-code
  cache, which is what the home page promises for warm starts.
- **2.3** As the test suite, then a DAP test attaches to a running tour and
  stops at a breakpoint, and a build-tool test runs an emitted `.cja`.

## 3. Acceptance
`toolchain/debugger-attach` attaches; `toolchain/startup-cja-*` run a `.cja`.
