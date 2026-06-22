---
id: cajeta-driver-jit-run
applies-to: [cajeta-driver/jit-run]
title: cajeta jit-run — compile a source tree and JIT-run an entry method in-process
description: Compiles every .cajeta under a source root to in-memory IR and runs a static no-arg entry method inside the cajeta process; the entry's int32 return becomes the exit code.
---

# cajeta jit-run

Compile a Cajeta project to in-memory LLVM IR (runtime + stdlib merged), build an
LLJIT, and run one chosen entry method **inside this same `cajeta` process**. A
developer/diagnostic verb that exercises the JIT host headlessly; `cajeta dap`
reuses the same `runJit()` host.

## Invocation

```
cajeta jit-run [-g] <source-root> <package.Class.method> [program-args...]
```

```sh
# samples/jit-smoke/demo/Hello.cajeta: public static int32 main() { return 6 * 7; }
$ cajeta jit-run samples/jit-smoke demo.Hello.main
[jit-run] entry demo.Hello.main returned 42
$ echo $?
42
```

## I/O contract

- `<source-root>` — directory whose `.cajeta` files form the compilation unit.
  Every file's path-derived package must match its `package` declaration (normal
  compiler convention). There is no single-file mode; you point at the root.
- `<package.Class.method>` — entry in dotted form. Must be a **static,
  parameter-less** method. Internally mangled to `package.Class::method`.
  - returns `int32` → that value is the process exit code; prints
    `[jit-run] entry <m> returned <n>` to **stderr**.
  - returns `void` → exit code 0; prints `[jit-run] entry <m> completed (void)`.
- `[program-args...]` — **accepted but NOT forwarded.** The entry is parameter-less
  for now; trailing args are parsed and held but never reach the program. Do not
  expect `argv`-style access yet.
- Program stdout/stderr go straight to this process's stdout/stderr (not captured).

## Flags

- `-g` / `--debug-info` / `--debug-info=on` — emit `__cajeta_dbg_safepoint` polls +
  debug frames (otherwise off). `--debug-info=off` forces it off.
- Flags may appear anywhere in the arg list; the parser filters them out and treats
  the remaining tokens as positionals in order (root, then entry, then args).

## Exit codes

- `2` — usage error: fewer than two positionals. Prints
  `usage: cajeta jit-run [-g] <sourceRoot> <package.Class.method> [args...]`.
- `1` — entry symbol lookup failed (e.g. wrong/misspelled `package.Class.method`,
  non-static, or has parameters so the mangled name doesn't match).
- non-zero — compile/JIT/build failure (propagated from the build step); diagnostics
  to stderr.
- `0` or the int32 return — success.

## What it does NOT do

- Does not write any artifact (no `.o`/`.exe`/`.cja`) — execution is purely
  in-memory. For build outputs use the compiler's `--emit=` path, not `jit-run`.
- Does not pass program args to the entry (see above).
- Does not take compiler mode/optimization flags here; it builds the project with
  the JIT host's own settings. `-g` is the only behavioral flag.
- Does not start a debugger UI. For breakpoints/stepping use `cajeta dap`
  (the DAP server), which drives the same host.

## Source

`src/cajeta/jit/CajetaJitHost.{h,cpp}` — `dispatchJitRun()` (arg parsing/exit
codes), `runJit()` (compile→JIT→invoke), `entryTargetFromDotted()` (name mangling).
Dispatched from `src/main.cpp` when `argv[1] == "jit-run"`.
