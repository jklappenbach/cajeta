---
id: stdlib-overview
applies-to: [cajeta.stdlib, cajeta/stdlib]
title: The cajeta standard library — task→package router
description: One table from what you are trying to do to the stdlib package that does it, each row pointing at that package's own overview skill; plus what the stdlib does not provide.
---

# The cajeta standard library — where to go

The stdlib is **built into the toolchain**: never declare it in
`settings.dependencies`, never fetch it; dead-code elimination links only what
you use (`cajeta/toolchain/project`). Every package below ships its own skills
— this page routes, the package overview details.

## Task → package

| You want to… | Package | Start at |
|---|---|---|
| Core types: `Object`, `String`, boxed numerics, `Optional`, `Pair`, `Guid`, `Math`, streams, `System.stdout` | `cajeta.lang` | `lang-overview` |
| Read/write files, paths, buffers, streaming | `cajeta.io` | `io-file-overview`, `io-overview` |
| TCP/UDP sockets, servers, DNS, TLS, URIs, the reactor | `cajeta.io.net` | `io-net-overview` |
| Lists, maps, sets, heaps, sorting, caches, B+trees | `cajeta.collection` | `collection-overview` |
| Fibers, locks, mutexes, channels, atomics, `FiberLocal`, `Tasks` | `cajeta.concurrent` | `concurrent-overview` |
| Exceptions, the `Throwable` hierarchy, diagnostics | `cajeta.error` | `error-overview` |
| Instants, durations, calendars, zones, formatting | `cajeta.time` | `time-overview` |
| JSON, CSV, Base64 (codecs in-tree) | `cajeta.codec` | `codec-overview`, `codec-json-overview` |
| Hashing + digests (SipHash, XXHash3, SHA-256) | `cajeta.hash` | `hash-overview` |
| Matrices, tensors, geometry, transforms, dtypes | `cajeta.math` | `math-overview` |
| Dataframes, sparse matrices, the ML/numeric stack | `cajeta.nucleo` | `nucleo-frame-overview` |
| Spawning subprocesses, capturing stdout/stderr | `cajeta.process` | `process-overview` |
| Runtime type info, annotations, dynamic invoke | `cajeta.reflect` | `reflect-overview` |
| Fuzzy matching, edit distance, n-gram indexes | `cajeta.search` | `search-overview` |
| Compression + wire codecs, schemas | `cajeta.wire` | `wire-overview` |
| GPU/accelerator kernels, buffers, cooperative matrix, ray query | `cajeta.xpu` | `xpu-overview` |
| Textures and graphics resources | `cajeta.gfx` | `gfx-textures` |
| Windows, input, audio playback/recording, backends | `cajeta.ifx` | `ifx-overview` |

## Not in the stdlib

- **An HTTP client/server, or WebSockets** — these live in the external
  `cajeta-http` library, not in-tree. Don't invest in the local http/ws code.
- **A test framework** — cajeta-unit is an external dependency
  (`cajeta/toolchain/testing`).
- **Columnar/interchange codecs** (Protobuf, Ion, Avro, Parquet, ORC) — the
  separate `cajeta-codec` library. In-tree `cajeta.codec` is JSON/CSV/Base64.
- **Logging** — the external `cajeta-logging` library (`@Logged`).
- **DI/aspects** — those are *language* features, not a library
  (`cajeta/language/annotations`).
- **Raw OS threads** — you spawn fibers, never threads
  (`cajeta/language/concurrency`).

## Rules that hold across the library

Ownership conventions (`#` transfer at call sites, owned vs borrowed returns,
who frees) are the language's, not each package's — read
`cajeta/language/ownership` once and apply it everywhere. Two library-wide
patterns worth knowing before you start:

- **Returns are `Optional<T>`, not null**, in the modern packages (channels,
  timeouts, lookups) — check `isPresent()` before `get()`.
- **Failures throw; there is no `Result` type.** Resource cleanup rides the
  drop chain — no try-with-resources (`cajeta/language/errors`).

**Collections do not own their elements** — an `ArrayList<T>` of heap values
does not free them for you; the owning-collection story is per-container, so
check the container's skill before assuming.
