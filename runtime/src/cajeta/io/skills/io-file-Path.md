---
id: io-file-Path
applies-to: [cajeta/io/file/Path]
title: Path — immutable filesystem path, pure-path queries (no syscalls in Phase B)
description: Build with Path.of(String) (#Path, copies bytes); name/stem/extension/parent/resolve are pure string ops — no exists/isFile/canonical in Phase B.
---

# Path

An **immutable filesystem path value** in the `cajeta.io.file` package. It wraps
the raw OS-path bytes (`int8[]`, UTF-8 octets, `/` separator on Linux) and answers
**pure-path queries** — `name`, `stem`, `extension`, `parent`, `isAbsolute` — plus
the join op `resolve`. **Phase B is string math only: it touches no filesystem.**

**Access point:** yes. You obtain one from the static factory `Path.of(String)`.

## Construction & ownership

```cajeta
import cajeta.lang.String;
import cajeta.io.file.Path;

#Path p = Path.of("/foo/bar/baz.txt");   // owned by caller
```

- `static #Path of(String s)` — **the entry point.** **Copies** `s`'s bytes into a
  fresh `int8[]` owned by the new Path, so the source `String` can drop
  independently (the factory does NOT take `s`). Returns an **owned `#Path`**.
- `Path(int8[] bytes)` — low-level constructor that **takes ownership** of `bytes`
  (`#` transfer). Most code uses `Path.of` instead; reach for `heap Path(#raw)`
  only when you already hold encoded OS bytes.
- Backing `int8[] bytes` is owned by the Path and freed by the auto-field-drop
  chain when the Path drops — **no `close()`, no explicit free** (see §Lifecycle).

## Methods that matter

Returns marked `#` are **freshly heap-allocated and owned by the caller**; none
are nullable in Phase B.

- `boolean isAbsolute()` / `boolean isRelative()` — leading `/` test; an empty path
  is relative.
- `#String name()` — last segment incl. extension. `/foo/bar/baz.txt` → `"baz.txt"`;
  `/` → `""`.
- `#String stem()` — `name()` minus the last `.ext` (single strip).
  `archive.tar.gz` → `"archive.tar"`; `passwd` → `"passwd"`.
- `#String extension()` — text after the rightmost dot, exclusive, no leading dot.
  `data.json` → `"json"`; `README` → `""`; a leading-dot dotfile `.bashrc` → `""`.
- `#Path parent()` — everything before the last `/`. `/foo` → `/`; `/` → `/`;
  bare `name` → `""` (empty path).
- `#Path resolve(String segment)` — **join one step.** Inserts a single `/` only
  when needed. `segment` is taken **whole — embedded slashes are NOT split**
  (pathlib semantics). This is the named surface for the spec's `operator/`, which
  isn't available yet.

```cajeta
#Path src = Path.of("src");
#Path f   = src.resolve("lang").resolve("Type.cajeta");   // "src/lang/Type.cajeta"
String leaf = f.name();        // "Type.cajeta"
String ext  = f.extension();   // "cajeta"
#Path dir   = f.parent();      // "src/lang"
```

## Lifecycle, state & concurrency

- **Immutable** — every query returns a new value; the receiver is never mutated.
  No reuse hazard; freely re-query.
- **No disposal protocol.** A Path frees its bytes on drop; there is nothing to
  close. Returned `#String`/`#Path` values are owned — let them drop or move them.

## Errors

Phase B raises nothing — these are pure byte operations over the stored array.

## What Path does NOT do in Phase B (don't hunt for it)

- **No syscalls / no filesystem access.** `exists` / `isFile` / `isDir` /
  `isSymlink` / `info` / `canonical` are **not part of this surface** — the stat
  predicates present in the source are non-functional stubs (return `false` / empty)
  awaiting Phase C intrinsic lowering. Do not branch on them.
- **No mutation.** `mkdirs` / `delete` / `setExecutable` / `symlinkTo` are Phase D
  stubs and currently no-op. There is no `mkdir -p` here yet.
- **No `operator/`.** Use `resolve(segment)` for joins until the grammar accepts a
  `#`-returning operator method.
- **No path normalization** of `.`/`..` or redundant slashes, and no Windows
  separator handling.
