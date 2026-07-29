# Debugging a compiled binary with gdb

`cajeta dap` debugs a program the compiler is running. This page is about the
other case: a native binary produced by `cajeta build`, opened in gdb, with no
compiler anywhere.

## Why gdb alone shows you nothing

Cajeta emits no DWARF. There is no `.debug_*` section in any artifact, and no
`DIBuilder` in the compiler. So gdb's own verbs cannot help you:

```
(gdb) break Tour.cajeta:53
No source file named Tour.cajeta.
(gdb) info locals
No symbol table info available.
```

That is by design, not an omission. DWARF is a host-only format in practice, and
it cannot express the three things this language most needs a debugger to show:
**ownership**, **allocation kind**, and **drop state**. Cajeta already emits its
own encoding — a location table, a shadow stack, a debug frame chain, and RTTI —
and that encoding works identically under the JIT, in an AOT binary, and on
device targets. One mechanism serves every target. A DWARF path would be a second
one to keep in sync, for the host only.

The bridge script reads that encoding and gives gdb a set of `cj*` commands.

## Building for it

`--debug-info` has three levels:

| Level  | What the binary carries |
|--------|--------------------------|
| `off`  | Nothing. |
| `line` | The shadow stack and frame descriptors, so an exception trace still resolves to `Type.method(File.cajeta:NN)`. No safepoints, no local records. |
| `full` | Everything above, plus per-statement safepoints, local records, the embedded location table, and forced RTTI retention. This is what the debugger needs. |

`line` is the default, and what the `release` flavor uses. The `debug` flavor
uses `full`, so an ordinary `cajeta build` is already debuggable:

```
cajeta build                          # debug flavor -> --debug-info=full
cajeta build --flavor=release         # -> --debug-info=line
```

Straight from the compiler:

```
cajeta --debug-info=full --emit=exe demo.Hello.run src/ build/
```

### What each level costs

Measured on `samples/tour`, `--emit=exe`:

| Level  | Binary | vs `off` |
|--------|--------|----------|
| `off`  | 5.3 MB | — |
| `line` | 6.3 MB | +19% |
| `full` | 14.4 MB | +172% |

`full` is large because it retains RTTI for every class — a debugger has to be
able to decode a local of any type, including types the program itself never
reflects on. That is the trade: `line` is cheap and gives you readable exception
traces in production; `full` is for when you are going to attach a debugger.

## Loading the bridge

The script installs alongside the compiler:

```
gdb -x /usr/share/cajeta/gdb/cajeta_gdb.py ./build/a.out
```

From a source checkout, point at `tools/gdb/cajeta_gdb.py`.

## The commands

### `cjbreak File.cajeta:NN`

Break at a source line. The bridge resolves the line to its statement ids through
the embedded table, then arms a conditional breakpoint on the safepoint those
statements call.

```
(gdb) break main
(gdb) run
(gdb) cjbreak Tour.cajeta:53
cjbreak Tour.cajeta:53 -> breakpoint 2 on 1 statement(s)
(gdb) continue
```

`cjbreak` needs the program running, because it reads the location table out of
the inferior. Break on `main` and `run` first, as above.

Two statements on one line get two ids, and both stop. A line that emits no code
— a blank line, a comment, a bare declaration — has no id, and `cjbreak` tells
you so rather than arming a breakpoint that never fires.

### `cjstack`

The live Cajeta stack. Every frame names its file and line, including stdlib
frames.

```
(gdb) cjstack
  #0  cajeta.collection.ArrayList<tour.DemoClass>.add(cajeta/collection/ArrayList.cajeta:183)
  #1  tour.Tour.main(tour/Tour.cajeta:53)
```

### `cjlocals [depth]`

The current frame's locals. For each: its name, its **declared** type, the
**dynamic** type of what it actually holds, its fields decoded from the RTTI's
byte offsets, and its three memory facets.

```
(gdb) cjlocals
  s : demo.Shape  (dynamic: demo.Circle)  [heap, owner, about-to-drop]
        .radius @+16 = 2.5
      inherited from demo.Shape:
        .sides @+8 = 7
  greeting : cajeta.lang.String  [heap, owner, about-to-drop]
      "hello debugger"
  args : cajeta.lang.String[]  [heap, borrow, live]
      <cajeta.lang.String[], 1 element(s)>
```

The bracketed facets are where the value lives, who is responsible for it, and
its lifetime state **at this stop**. A local that has been moved out of says so,
instead of showing you the stale value:

```
  owned : demo.Box = <moved-from — ownership transferred out>  [heap, owner, moved-out]
```

That last line is the reason this encoding exists. The ownership *role* is static
— an owner that `#` has moved out of is still declared an owner — so the live
signal comes from the drop-chain entry, which the transfer deactivated. A
DWARF-driven debugger has nowhere to put that fact, and would print the consumed
value as though it were live.

Reference fields are followed to a depth of 3 by default; pass a depth to go
deeper. A cyclic object graph terminates and marks the revisited node.

### `cjstep` and `cjnext`

`cjstep` runs to the next Cajeta **statement**, not the next machine instruction.
`cjnext` does the same but stays in the current frame, stepping over calls.

```
(gdb) cjstep
demo.Hello.run(demo/Hello.cajeta:15)
(gdb) cjstep
demo.Hello.run(demo/Hello.cajeta:16)
```

### `cjlist [n]`

Source around the current line. The location table stores paths relative to the
build root, so run gdb from there.

## When it says to rebuild

Every command checks that the binary actually carries what it needs:

```
(gdb) cjlocals
cjlocals needs a binary built with --debug-info=full; this one carries no debug
records. Rebuild with `cajeta build --flavor=debug` (or pass --debug-info=full)
and try again.
```

A `line` build is the in-between case: `cjstack` works, because the shadow stack
is there, but there are no safepoints to break on and no local records to read.

## Limits

- Reading only. Setting a value from gdb is not supported.
- `release` builds are optimized; inlining and reordering will make stepping
  confusing. Debug an unoptimized build.
- `cjbreak` reads the ABI's first integer-argument register, so it supports
  x86-64 and aarch64.
- A static field (byte offset -1) lives in a global, not in the instance;
  `cjlocals` reports it as unsupported rather than decoding a bogus offset.
