# 05 — Debugging

Cajeta binaries are native, so native tooling works. The toolchain adds a
Debug Adapter Protocol (DAP) server for IDE debugging.

## Build for debugging

Debug is the default mode (`--mode=debug`). It turns on bounds checks, null
checks, drop chain validation, and source positions. Release builds strip
these, so debug what you build with `cajeta build` and profile what you build
with `release`. See
[compiler modes](../specification/buildtool/CompilerModes.md).

## gdb

The binary carries standard debug info:

```bash
$ cajeta build
$ gdb build/exe/com.example.basic
(gdb) rbreak Main::main
(gdb) run
```

Symbols carry their full Cajeta signatures
(`com.example.basic.Main::main(args:cajeta.lang.String[])`), so
`rbreak <regex>` is the convenient way to set breakpoints. Quoting the full
signature works too. Stepping and stack traces behave as with any native
binary. Cajeta-specific state, such as fibers and drop chains, is easier to
inspect in a DAP client.

## The DAP server

`cajeta dap` speaks Debug Adapter Protocol over stdio. Any DAP-capable editor
can drive it, with breakpoints, stepping, variable inspection, and
break-on-throw. Two features are Cajeta-specific today. Every fiber appears as
a thread, covering the stopped fiber plus all live fibers' chains, and each
variable carries its ownership role as an annotation. Drop-chain inspection
and capability-violation breakpoints are designed but not yet implemented. See
[the debugging design](../specification/debugging/Debugging.md).

## IntelliJ

The toolchain bundles an IDEA plugin:

```bash
$ cajeta ide install     # ide list / ide uninstall to manage
```

Open the project directory. Build tasks route to the Build window, and the
debugger runs over the bundled DAP integration.

## VS Code

Point any generic DAP client extension at `cajeta dap`. The launch
configuration has this shape:

```json
{
    "type": "cajeta",
    "request": "launch",
    "program": "${workspaceFolder}/build/exe/com.example.basic"
}
```

See [the debugging design](../specification/debugging/Debugging.md) for the
supported requests and the launch-configuration reference.

Next: Part II, the language itself. See the [guide index](README.md) for
chapter status.
