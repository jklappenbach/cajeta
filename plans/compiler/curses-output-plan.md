# Compiler enhanced output (curses) plan

Source: `plans/current-focus.md` → **Compiler → Enhanced output (curses) to
provide completion feedback**.

> **Status at authoring:** the compiler emits plain text — diagnostics to
> `std::cerr`, logging to `std::cout` (`src/main.cpp`). No progress UI, no
> terminal control, no curses. The build tool (`src/cajeta/buildtool/`) has
> task-level progress notions but no rich rendering.

Goal: a richer terminal experience that shows **compilation progress and
completion feedback** as it happens — phase/stage progress, per-module status,
a live summary — degrading gracefully to plain text when stdout isn't a TTY (CI,
pipes, the IDE).

This is a **scaffold**; the rendering approach and scope are Julian's call.

## Goals

- [ ] Live progress while compiling: which phase (parse → type → codegen → link),
      how many modules done / total, current file.
- [ ] Completion feedback: success/failure summary, counts (modules, warnings,
      errors), elapsed time.
- [ ] Clean, legible diagnostics (errors/warnings) that coexist with the live
      progress region without scrambling it.
- [ ] **Graceful degradation:** detect non-TTY (`isatty`) → fall back to the
      current line-oriented plain output. Never emit escape codes into a pipe,
      a log file, or the IDE's captured stream.

## Hard constraints

- **Must not break machine-readable output.** The IntelliJ plugin needs
  structured diagnostics (`--lint --diag-format=json`, per
  `ide-plugins/idea/Plan.md` §7). The curses UI is a **human-TTY-only** layer; a
  `--no-progress` / `--diag-format=json` path must bypass it entirely.
- Cross-platform: Linux/macOS (ANSI) and Windows (the release targets include
  `x86_64-w64-mingw32`). Windows terminal handling differs — see D2.
- The compiler already links a fixed set of libs (LLVM, antlr4, zstd, glog,
  OpenSSL); adding a curses dependency affects the installer/release packaging
  (`plans/installer/installer-plan.md`) — weigh in D1.

## Open decisions (need Julian)

- **D1. Rendering approach.** Full **ncurses** (new system dependency, alternate
  screen) vs lightweight **ANSI escapes** hand-rolled (no dependency, in-place
  line updates `\r` / cursor-up) vs a small vendored TUI helper? Lean: **ANSI
  escapes, no ncurses** — avoids a packaging dependency across 4 release targets
  and is enough for a progress region + summary. Confirm.
- **D2. Windows terminal strategy.** Enable VT processing
  (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`) on modern Windows terminals, else plain
  fallback. Acceptable?
- **D3. Scope: compiler only, or build-tool too?** The build tool runs tasks
  (which invoke the compiler). Should the progress UI live at the **build-tool**
  layer (task/action progress, the thing users actually watch) with the compiler
  just emitting structured progress events the tool renders? That's the cleaner
  separation and reuses one renderer. Strong lean: **compiler emits progress
  events; build tool (and IDE) render.** Confirm before building a curses layer
  inside the compiler itself.
- **D4. Interaction with `glog`.** Logging currently goes through glog; the
  progress region must not be trampled by stray log lines. Route logs above the
  live region, or suppress below a verbosity threshold when the TTY UI is active?

## First step

- [ ] Resolve D1 + D3 (they determine *where* the code lives and whether there's
      a new dependency), then write the phased plan: progress-event emission
      (structured, format-agnostic) → TTY renderer → non-TTY fallback →
      build-tool/IDE consumers.
