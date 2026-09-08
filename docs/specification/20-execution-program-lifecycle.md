# 20 — Execution & Program Lifecycle

This chapter defines a program's lifecycle — startup, initialization, execution, exit — and the rule that gives the rest of this specification its footing across deployment forms: **a Cajeta program's meaning is independent of its shape**. An archive under the JIT and a native binary are two lowerings of the same defined behavior.

## 20.1 Program Shapes

A program ships and runs in three shapes:

- **A `.cja` IR archive.** `cajeta build` emits a compressed archive carrying the program as LLVM bitcode (with sources, resources, and agent skills). At run time the JIT lowers the bitcode for the executing CPU and caches the machine code, so a warm start skips compilation. One artifact runs on every platform the toolchain supports.
- **A native executable.** `--emit=exe` compiles the same IR ahead of time for a specific target triple.
- **A static library.** `--emit=staticlib` produces a linkable artifact with a C-ABI surface for embedding in a host application.

A conforming implementation gives an accepted program identical observable behavior in every shape. This specification defines no way for a program to detect its shape, and programs must not rely on incidental differences; any future shape-detection surface would be specified here first.

> *Discussion.* Two shape-adjacent behaviors are implementation properties, not semantics: *when* the JIT materializes a method's machine code (caching makes it early on a warm start), and *which* runtime and stdlib code is linked — linking is lazy, materializing only what the program references, and a program cannot observe the difference because linking has no side effects. Programs must not depend on compilation timing.

## 20.2 Startup and Initialization

Execution begins at the entry point: `static int32 main(String[] args)` (or `main()`) in a class program, or the synthesized entry of a script unit (Script Units §18). The argument store is installed by the host before entry — `System.args` and a `main` parameter read the same store (Script Units §18.3) — and the environment is reachable through `System.env`.

Static initializers run at program start, before the entry point's first statement, not lazily on first use of the class.

## 20.3 Exit

A program exits when the entry point returns — the return value is the exit code; `void` and script units without a top-level `return` exit 0 — or when an `UnrecoverableException` terminates it (Errors §15.2), or abnormally when the runtime detects a fatal condition.

Drop guarantees at the end of life:

- On the normal path, owning locals drop at their blocks per Allocation §4, and session bindings drop at session end in reverse binding order (Script Units §18.2).
- On termination by unrecoverable throw, the drop chain unwinds before the process ends.

## 20.4 Execution Environments

The three shapes are joined by two interactive hosts over the same JIT: `cajeta run` for script units (Script Units §18) and the notebook kernel (Notebook Kernel §19). Program semantics in every host are as this specification defines; hosts differ only in entry synthesis, argument installation, and session lifetime, each specified in its own chapter.
