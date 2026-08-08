# 00 — Introduction

Cajeta is a compiled language with Java-shaped syntax, C++-grade native output,
and Rust-style memory safety without a garbage collector. Programs compile
through LLVM to real binaries. Memory is managed by ownership: `=` borrows, `#`
transfers, and the compiler proves the rest.

A first taste:

```cajeta
package hello;

import cajeta.lang.System;

public class Main {
    public static void main(String[] args) {
        System.stdout.println("hello, cajeta");
    }
}
```

What comes with the toolchain:

- One binary, `cajeta` — compiler, build tool, package manager, doc generator,
  and debug server.
- A standard library covering collections, streams, fibers and async,
  file and network I/O, time, hashing, codecs, math, and GPU kernels.
- Compile-time dependency injection and aspects — no runtime container.
- A capability model: a program touches only what its manifest declares.
- Built-in support for AI agents: `cajeta compiler-mcp` is a Model Context
  Protocol server inside the compiler, serving **skills** — hand-written
  implementation guidance for the language, the toolchain, and every library.
  More than 180 ship in the binary, and every published library carries its own
  ([chapter 03](03-your-first-project.md)).

## How the documentation fits together

- **This guide** — a linear walk: install the toolchain, build a project, then
  the language and standard library in order. Read it front to back.
- **[Standard library reference](../stdlib/README.md)** — one page per class,
  organized by package. Go here for full method surfaces.
- **[The tour](../../samples/tour/)** — runnable demos of every feature.
  Chapters link to the tour file that demonstrates what they teach.
- **[Language specification](../specification/)** — design-level reference.

Next: [Installation](01-installation.md).
