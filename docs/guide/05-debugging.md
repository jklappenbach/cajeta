# 05 — Debugging

Cajeta binaries are native, so native tooling works; the toolchain adds a
Debug Adapter Protocol (DAP) server for IDE debugging.

## Build for debugging

Debug is the default mode (`--mode=debug`): bounds checks, null checks, drop
chain validation, and source positions all on. Release builds strip these —
debug what you build with `cajeta build`, profile what you build with
`release`. See [compiler modes](../specification/buildtool/CompilerModes.md).

## gdb

The binary carries standard debug info:

```bash
$ cajeta build
$ gdb build/exe/com.example.basic
(gdb) rbreak Main::main
(gdb) run
```

Symbols carry their full Cajeta signatures
(`com.example.basic.Main::main(args:cajeta.lang.String[])`), so `rbreak
<regex>` is the convenient way to set breakpoints; quoting the full signature
works too. Stepping and stack traces behave as with any native binary.
Cajeta-specific state (fibers, drop chains) is easier in a DAP client.

## The DAP server

`cajeta dap` speaks Debug Adapter Protocol over stdio. Any DAP-capable editor
can drive it: breakpoints, stepping, variable inspection, plus
Cajeta-specific views — the fiber pane, drop-chain inspection, ownership
annotations on variables, and capability-violation breakpoints. The full
surface is specified in
[the debugging design](../specification/debugging/Debugging.md).

## IntelliJ

The toolchain bundles an IDEA plugin:

```bash
$ cajeta ide install     # ide list / ide uninstall to manage
```

Open the project directory; build tasks route to the Build window, and the
debugger runs over the bundled DAP integration.

## VS Code

Point any generic DAP client extension at `cajeta dap`. Launch configuration
shape:

```json
{
    "type": "cajeta",
    "request": "launch",
    "program": "${workspaceFolder}/build/exe/com.example.basic"
}
```

See [the debugging design](../specification/debugging/Debugging.md) for the
supported requests and the launch-configuration reference.

Next: Part II — the language itself. See the [guide index](README.md) for
chapter status.
