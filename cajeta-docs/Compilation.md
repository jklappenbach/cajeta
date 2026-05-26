# Compilation.md

Specification for how cajeta source becomes a runnable program: the
source-tree layout the compiler expects, the phases the compiler
runs, the artifacts it produces, the flags that control each step,
and the binary / archive targets supported.

This document is the contract between the cajeta toolchain and its
users. Behaviors named here are stable across point releases; design
docs in `cajeta-docs/` (stdlib/* and stdlib/Reflection.md, etc.)
describe what the *content* of those artifacts is — this one
describes the *machinery*.

## Table of contents

1. [Source tree structure](#source-tree-structure)
2. [Compilation phases](#compilation-phases)
3. [Output formats](#output-formats)
4. [Archive format](#archive-format)
5. [Uber archives](#uber-archives)
5. [Resources](#resources)
6. [Binary releases](#binary-releases)
7. [Optimization](#optimization)
8. [Compiler flag index](#compiler-flag-index)

---

## Source tree structure

Project layout, Maven-style with cajeta naming:

```
<project>/
├── cajeta.toml                 # project manifest (name, version,
│                                #   dependencies, target triple
│                                #   defaults, build flavors)
├── src/
│   ├── main/
│   │   ├── cajeta/             # production .cajeta source
│   │   │   └── com/example/
│   │   │       ├── User.cajeta
│   │   │       └── Service.cajeta
│   │   └── resources/          # production embedded resources
│   │       ├── templates/
│   │       │   └── greeting.html
│   │       └── config/
│   │           └── default.yaml
│   └── test/
│       ├── cajeta/             # test .cajeta source
│       │   └── com/example/
│       │       └── UserTest.cajeta
│       └── resources/          # test-only embedded resources
│           └── fixtures/
│               └── golden.json
├── lib/                        # local / vendored archive dependencies
│   └── <archive>.cja
└── build/                      # compiler output (gitignored)
    ├── ir/                     # --emit=ir output
    ├── obj/                    # --emit=obj output
    ├── exe/                    # --emit=exe output
    └── archive/                # --emit=archive output
```

**`src/main/cajeta/` and `src/test/cajeta/`** — package directories
mirror the canonical name. `com.example.User` lives at
`src/main/cajeta/com/example/User.cajeta`. The compiler's
package-path check (`onPackageDeclaration`) enforces this.

**`src/main/resources/` and `src/test/resources/`** — bundled into
the output archive at parallel paths (or linked into the binary as
embedded blobs for `--emit=exe`). Test resources are accessible
only to test code; production resources are accessible to both.
See [Resources](#resources) for the runtime API.

**`cajeta.toml`** — project manifest. Names the project, declares
version + cajeta language version, lists archive dependencies
(local paths or registry references), pins default target triples,
and configures build flavors (release, debug, fast). Driven by the
`cajeta` build tool (`cajeta build`, `cajeta test`, etc.) which
wraps the compiler. See "Build tool integration" below.

**`lib/`** — local archive dependencies. The cajeta compiler's
`--classpath` flag points at this directory (or specific archives
within); user code references types defined in those archives by
canonical name, resolved at compile time against the archives'
embedded class metadata.

The Maven `src/main/<lang>` convention exists for reasons that
apply to cajeta too: cleanly separates production from test, gives
resources their own first-class root (no scattered `.txt` files
mixed with source), and survives multi-language projects (a future
`src/main/c/` for FFI shims wouldn't break anything).

---

## Compilation phases

```
.cajeta source
    │
    ▼
┌──────────────┐
│ 1. Parse     │   ANTLR4-generated lexer + parser. Builds an
│              │   AST per file.
└──────────────┘
    │
    ▼
┌──────────────┐
│ 2. Resolve   │   Class registry, type lookups, generic
│              │   instantiations, deferred-prototype sweep
│              │   for cross-file forward references.
└──────────────┘
    │
    ▼
┌──────────────┐
│ 3. Lower     │   AST → LLVM IR. Per-class layout,
│              │   vtables, RTTI, method body codegen. Cross-
│              │   module extern decls for stdlib references.
└──────────────┘
    │
    ▼
┌──────────────┐
│ 4. Optimize  │   LLVM optimization passes per -O level.
│              │   Optional LTO. Optional PGO instrumentation
│              │   or use.
└──────────────┘
    │
    ▼
┌──────────────┐
│ 5. Emit      │   Depending on --emit: write .ll text IR,
│              │   .o native object, .cja archive, or linked
│              │   executable.
└──────────────┘
```

**Phase 1 — Parse.** `.cajeta` files in `src/main/cajeta/` (or the
explicit source root) are walked, lexed, parsed. Cross-file
forward references are recorded as placeholders; the archive
pre-scan registers every declared class name before any module
parses, so a class referenced before its declaration parses works.

**Phase 2 — Resolve.** Class registry (canonicalMap) populated.
Generic instantiations created on demand. Deferred prototypes
build to fixed point: classes whose superclass was a placeholder at
visit time get their layout deferred until the parent's
prototype lands. The post-parse sweep walks until no class can
make further progress.

**Phase 3 — Lower.** Each method becomes an LLVM function. Class
layouts become LLVM aggregate types. Vtable globals + RTTI globals
get emitted. Cross-module references — calls to stdlib methods,
references to stdlib vtables — go through `ensureFunctionInModule`
/ `ensureGlobalInModule` to insert extern declarations in the
calling module; the merge step resolves them to definitions when
the archive's bitcode joins user IR.

**Phase 4 — Optimize.** LLVM passes per `-O` level. The cajeta
compiler runs the standard LLVM pass pipeline plus a few cajeta-
specific passes (drop-chain elimination, vtable devirtualization
for sealed classes, structural-hash inlining). LTO and PGO are
opt-in via flags.

**Phase 5 — Emit.** Output format set by `--emit`:

- `--emit=ir` — exploded text LLVM IR (`.ll`), one file per module under a directory tree mirroring the source-tree package layout.
- `--emit=obj` — exploded native objects (`.o` / `.obj`), same directory shape.
- `--emit=archive` — single cajeta archive (`.cja`) bundling every compiled module's bitcode + a manifest. Thin form — user modules only; reads dependencies from the classpath at consumption time. See [Archive format](#archive-format).
- `--emit=uber` — single cajeta archive (`.cja`) bundling the user's modules AND every dependency archive on the `--classpath` that user code transitively references. Java's "uber-jar" equivalent. Same `.cja` extension; the manifest's `kind` field distinguishes thin from uber. See [Uber archives](#uber-archives).
- `--emit=exe` — linked executable, native target.

---

## Output formats

### `--emit=ir` (exploded text LLVM IR)

Human-readable LLVM `.ll` output per cajeta module, laid out as a directory tree mirroring the source-tree package layout. One `.ll` file per `.cajeta` source plus one for the stdlib (`cajeta.runtime.__stdlib__.ll`). Useful for debugging the compiler, inspecting generated IR, feeding into external LLVM tooling (`opt`, `llc`, `llvm-link`). Not intended for deployment.

```
build/ir/
├── cajeta.runtime.__stdlib__.ll       ← stdlib + embedded C runtime
└── <package>/<sub>/<Class>.ll         ← one per user source
```

### `--emit=obj` (exploded native object)

Native object files per module — ELF `.o` on Linux, COFF `.obj` on
Windows, Mach-O on macOS. Same directory shape as `--emit=ir`. Caller
links with a system linker. Useful when integrating cajeta code into
a larger build that already has its own linker invocation.

### `--emit=archive` (cajeta archive `.cja`, thin)

Single archive file containing every compiled class's bitcode, class metadata, embedded resources, and a manifest. **Thin form** — bundles user modules and parsed stdlib classes but NOT dependency archives (the consumer's compiler resolves those at compile time via `--classpath`). The distribution format for cajeta libraries — the stdlib itself ships as a `.cja`. See [Archive format](#archive-format) for the on-disk shape.

The manifest's `kind` field is `"thin"`.

### `--emit=uber` (cajeta archive `.cja`, uber)

Same `.cja` container as `--emit=archive`, but bundles user modules **plus** every dependency archive on `--classpath` that user code transitively references — Java's "uber-jar" pattern. Dependency entries land at the manifest's top level alongside user-code entries; the manifest's `kind` field is `"uber"`, and an `origins` map identifies which entries came from which source archive.

Used for **deployment**: one self-contained `.cja` you can hand to a runner without separately distributing the classpath. Adds storage size (every transitive dep is inlined); not appropriate for libraries (which want others to consume them as deps, not absorb them whole).

See [Uber archives](#uber-archives) for the spec.

### `--emit=exe` (linked executable)

Native executable for the configured target triple. Links the
user's compiled modules with the cajeta stdlib archive, the C
runtime, and any third-party archives on the classpath into a
single binary using the in-process `lld` (or the system linker if
lld isn't available). Embedded resources get baked into the
executable as `.rodata` segments addressable by path at runtime.

---

## Archive format

Cajeta archives use a custom container format with the file
extension `.cja` (Cajeta ARchive). The design priority is
**write-few-read-many**: a stdlib archive gets built once and
loaded by every cajeta program that runs against it, so
decompression speed and seekable random access matter more than
compression-time throughput.

### Compression algorithm

**zstd** (Zstandard), levels 19-22 for archive builds, level 3 for
incremental development builds.

zstd was picked over the alternatives because:

| Algorithm | Compression ratio | Decompression speed | Random access | Status            |
|-----------|-------------------|---------------------|---------------|-------------------|
| deflate (zip) | baseline       | ~500 MB/s           | per-entry     | Mature; the default for legacy. Worse than zstd on both axes. |
| gzip      | similar to deflate | ~400 MB/s          | stream only   | Same as deflate, plus checksum.                                |
| bzip2     | better than zstd at low ratios | ~80 MB/s | stream only | Obsolete; superseded by xz.                                  |
| xz (LZMA2) | best ratio       | ~80 MB/s            | stream only   | Best ratio, slow decode. Wrong tradeoff for read-many.       |
| lz4       | worst ratio       | ~3 GB/s             | per-block     | Fastest decode; ratio too low for an archive.                |
| brotli    | similar to zstd   | ~400 MB/s           | per-entry     | Web-optimized; less universal tooling.                       |
| snappy    | worse than lz4    | ~2 GB/s             | per-block     | Older; zstd dominates it now.                                 |
| **zstd**  | **competitive with xz at high levels** | **~1.5 GB/s** | **per-entry** | **Active development; format-stable since 2016; embedded in Linux kernel, Docker, deb / RPM, Btrfs, ZFS.** |

zstd at level 19 hits LZMA-territory compression with 10-20× faster
decompression. Per-entry compression means a reader can decompress
one class's bitcode without touching the rest of the archive —
exactly what cajeta needs (a compiler reading a stdlib archive
typically wants ~3-5 classes per user file, not the whole
hierarchy).

### On-disk layout

```
.cja file:
┌─────────────────────────────────────────────────┐
│ Magic       (8 bytes):  "CAJETA01"              │
│ Format ver  (4 bytes):  uint32 little-endian    │
│ Archive flags (4 bytes): bitmask                │
│ Index offset (8 bytes): uint64 → byte position  │
│ Index length (8 bytes): uint64 → byte count     │
├─────────────────────────────────────────────────┤
│ Manifest (zstd-compressed JSON)                 │
│   {                                             │
│     "name": "cajeta-stdlib",                    │
│     "version": "1.0.0",                         │
│     "cajeta_lang_version": "1.0",               │
│     "target_triple": null,                      │
│     "dependencies": [],                         │
│     "build_timestamp": "...",                   │
│     "build_flavor": "release"                   │
│   }                                             │
├─────────────────────────────────────────────────┤
│ Entry 1 (zstd-compressed)                       │
│   cajeta/error/Throwable.bc                     │
├─────────────────────────────────────────────────┤
│ Entry 2 (zstd-compressed)                       │
│   cajeta/error/Exception.bc                     │
├─────────────────────────────────────────────────┤
│ ... more entries ...                            │
├─────────────────────────────────────────────────┤
│ Resources (zstd-compressed)                     │
│   templates/greeting.html                       │
│   config/default.yaml                           │
├─────────────────────────────────────────────────┤
│ C runtime bitcode                               │
│   runtime/cajeta_runtime.bc                     │
├─────────────────────────────────────────────────┤
│ Index (zstd-compressed JSON)                    │
│   [                                             │
│     { "path": "cajeta/error/Throwable.bc",      │
│       "offset": 264,                            │
│       "compressed_size": 4321,                  │
│       "uncompressed_size": 12345,               │
│       "kind": "class_bitcode",                  │
│       "canonical_name": "cajeta.error.Throwable", │
│       "checksum_xxh3": "..." },                 │
│     ...                                         │
│   ]                                             │
└─────────────────────────────────────────────────┘
```

The index sits at the **end** of the file (not the start) so an
archive can be written in a single forward pass: write entries as
they're compressed, then write the index, then patch the header's
`index_offset` field. Readers seek to the end, read the index,
then random-access individual entries.

Format-version bumps are reserved for incompatible changes; backward-
compatible additions (new entry kinds, new manifest fields) don't
bump the version. The compiler refuses to load an archive whose
format version exceeds its supported maximum.

### Minimum-viable v1

The full container above (zstd, trailing index, resources, runtime-
bitcode block, separate manifest blob) is the design target. The
**v1 implementation lands a partial subset** so each remaining piece
can ship incrementally without blocking on the others:

- **Compression: zstd (level 3) on both manifest and entries by default.** Header flag bits 0 (`manifest_compressed`) and 1 (`entries_compressed`) flip when compression is on. Each compressed section is framed as `uint64 uncompressed_length || zstd_bytes`; the manifest_length / data_length values give the ON-DISK size, the uint64 prefix gives what to allocate before decompressing. Writer opt-out via `CajetaArchive::setCompression(Compression::None)`; the reader handles either path. CLI plumbing (`--archive-compress=on|off`) is a thin follow-up. Size win: HelloWorld's `.cja` goes from 418 KB → 173 KB (60% off) on stdlib-heavy content.
- **Trailing random-access index.** Written by every archive; readers can use it (via `header.index_offset != 0`) or skip it and scan sequentially. Format: `uint32 entry_count` followed by per-entry `(uint32 name_length, name bytes, uint64 entry_offset, uint64 entry_on_disk_size)`. `CajetaArchive::findEntry(name)` exposes O(1) lookup on the read side. The original spec's JSON-shape index sketched out in the design doc is reserved for a richer v2 (compressed_size, uncompressed_size, checksum_xxh3, kind metadata); v1's compact binary index is sufficient for the random-access use case.
- **No resources block, no runtime-bitcode block.** Only class bitcode entries for v1. Resources land alongside the `@Embedded` annotation work; the runtime bitcode block lands when `--emit=exe` migrates from the inline-stdlib-module shape to consuming a stdlib `.cja`.
- **Single-file output.** The writer produces one `.cja` per invocation. Multi-archive output (`--emit=archive --split` for per-package archives) is not on the v1 roadmap.

The header shape, magic bytes, format version field, manifest schema, and entry encoding ARE final in v1 — additions land via flag bits and manifest fields, not format-version bumps.

### Entry encoding (v1)

Each entry is length-prefixed and self-describing:

```
┌──────────────────────────────────────────────────────────────┐
│ name_length  (4 bytes, uint32 LE)                            │
│ name         (name_length bytes, UTF-8)                      │   e.g. "demo/App.bc"
│ origin_tag   (1 byte)                                        │   0=user, 1=stdlib, 2=dep
│ kind_tag     (1 byte)                                        │   0=class bitcode, 1=resource, 2=runtime bitcode
│ reserved     (2 bytes)                                       │   zero
│ data_length  (8 bytes, uint64 LE)                            │
│ data         (data_length bytes)                             │   raw bytes (LLVM bitcode for kinds 0/2, raw file for kind 1)
└──────────────────────────────────────────────────────────────┘
```

Names use forward-slash path separators regardless of host platform (matches `.jar` / `.zip` convention). Bitcode entries use `.bc` extension; resources keep their original extension.

`origin_tag` lets uber archives keep track of which dependency each entry came from at extraction time (the manifest's `origins` map gives the symbolic name; the tag is the categorical bucket). Thin archives have origin_tag=0 for every user entry; the parsed-stdlib classes go in with origin_tag=1.

### Internal structure mirrors the source tree

For a project with package `com.example`:

```
src/main/cajeta/com/example/User.cajeta    →  com/example/User.bc
src/main/cajeta/com/example/Service.cajeta →  com/example/Service.bc
src/main/resources/templates/greet.html    →  templates/greet.html
src/main/resources/config/default.yaml     →  config/default.yaml
```

Same path structure, just minus the `src/main/cajeta/` or
`src/main/resources/` prefix and with `.cajeta` → `.bc` for code
files. Resources keep their original filename + extension.

This mirroring is intentional: a developer reading the archive (via
`cja ls <archive>` / `cja cat <archive> <path>`) sees the same
layout they wrote, without having to map between source and
artifact paths.

### Random-access reading

Common reader pattern when the cajeta compiler loads an archive:

1. mmap the archive file
2. Read the header (constant offsets)
3. Decompress the index (small, ~kilobytes for the stdlib)
4. For each class referenced by user code:
   - Look up the canonical name in the index
   - Seek to the entry's offset
   - Decompress the bitcode (zstd is fast enough to do this lazily
     per class)
   - Hand the bitcode to LLVM

The index is cached in-memory for the compiler invocation, so
repeated lookups are O(1) hash table reads.

### CLI tools

```
cja create <archive> --manifest <file> --root <dir>
cja ls     <archive>
cja cat    <archive> <entry-path>
cja extract <archive> --out <dir>
cja verify <archive>          # check checksums + format
cja info   <archive>          # print manifest, entry count, sizes
```

The cajeta compiler embeds the writer; standalone `cja` ships as
part of the cajeta toolchain for inspection and repackaging.

---

## Uber archives

A `.cja` file produced by `--emit=uber` carries the user's modules PLUS every dependency archive on the `--classpath` that user code transitively references. Same container as a thin archive; the manifest's `"kind": "uber"` field and an `"origins"` mapping let consumers distinguish bundled deps from user code.

### Why uber

Java's `.jar` ecosystem has the same split. **Thin jars** are library archives — depend on other libraries at compile time, get assembled into a complete classpath at deployment time. **Uber jars** (also "fat jars" or "shaded jars") collapse a tree of dependencies into a single self-contained artifact — you hand the file to whoever's running it, they invoke a runner, done. No classpath wrangling, no version mismatches between user code and bundled deps.

Cajeta inherits the same rationale. Libraries publish as thin `.cja`s (`stdlib.cja`, `my-json-utils.cja`, …) so consumers can pin / replace specific versions. Applications publish as uber `.cja`s so deployment is one file. Microservices, CLI tools, sample programs, demos — all natural fits for uber.

### Manifest extensions

A thin manifest:

```json
{
    "name": "my-app",
    "version": "1.0.0",
    "kind": "thin",
    "cajeta_lang_version": "1.0",
    "dependencies": [
        { "name": "cajeta-stdlib", "version": "1.0.0" },
        { "name": "json-utils",     "version": "2.1.4" }
    ],
    "entry_method": "demo.App.run"
}
```

An uber manifest adds `origins`, listing where each entry came from:

```json
{
    "name": "my-app",
    "version": "1.0.0",
    "kind": "uber",
    "cajeta_lang_version": "1.0",
    "dependencies": [
        { "name": "cajeta-stdlib", "version": "1.0.0" },
        { "name": "json-utils",     "version": "2.1.4" }
    ],
    "entry_method": "demo.App.run",
    "origins": {
        "demo/App.bc":              "my-app",
        "cajeta/lang/Object.bc":    "cajeta-stdlib",
        "cajeta/lang/String.bc":    "cajeta-stdlib",
        "json/JsonReader.bc":       "json-utils",
        "json/JsonWriter.bc":       "json-utils"
    }
}
```

`origins` is the source-archive name from each dep's own manifest. The consumer can rebuild the original dep set if needed (for license-attribution generators, security scanners, etc.).

### Inclusion rules

Only **transitively referenced** dependency classes get bundled. The compiler runs a reachability analysis from the entry method and the user-code roots; classes that nothing references are excluded. This keeps uber archives lean — bundling `cajeta-stdlib` doesn't drag in JSON, hashing, parallel streams, etc. if user code doesn't use them.

The reachability set includes:

1. The `entry_method` and everything it transitively calls.
2. Every class user code declares a field, parameter, return type, or local of.
3. Every class user code's body references by name (including reflective uses via `@Reflect` annotation — Reflection.md).
4. Anything those classes recursively reference, transitively to fixed point.

Resources (`@Embedded`-annotated files, classpath-loaded YAMLs, etc.) are bundled separately — a `resources` flag on `--emit=uber` controls inclusion. Default: include resources from the user project, NOT from deps (deps include their own at consumption time via their own thin archive; uber doesn't re-bundle).

### Versioning + collisions

When two dependency archives expose the same canonical class name (`com.example.Util` in both `lib-a-1.0.cja` and `lib-b-2.0.cja`):

1. If the canonicals AND class-version metadata agree, dedupe (one entry covers both deps; `origins` lists both).
2. If they disagree, `--emit=uber` fails with `CAJETA_ERROR_UBER_CLASS_COLLISION` and lists every conflicting entry. The user resolves by upgrading one dep, excluding via `--classpath-exclude`, or repackaging via the `cja` tool's `shade` subcommand (renames a namespace).

The collision behavior is conservative on purpose: silently picking one resolution would break the consumer when the wrong version's API gets called.

### On-disk layout

Same container as a thin archive — just larger and with the manifest extensions above. A reader doesn't need to know it's an uber until it reads the manifest's `kind` field; thin and uber readers are interchangeable for the actual bitcode extraction.

```
my-app-uber.cja:
├── header  (CAJETA01 magic + version + flags + kind=1 (uber))
├── manifest (zstd-compressed JSON; "kind": "uber" + "origins")
├── demo/App.bc            ← from my-app
├── cajeta/lang/Object.bc  ← from cajeta-stdlib
├── cajeta/lang/String.bc  ← from cajeta-stdlib
├── json/JsonReader.bc     ← from json-utils
├── json/JsonWriter.bc     ← from json-utils
├── resources/             ← user-project only
│   └── templates/greet.html
└── index (zstd-compressed JSON pointing at all the above)
```

### Trade-offs

| | Thin | Uber |
|---|---|---|
| Use case | Library | Application |
| File size | Small (user code only) | Large (user + deps) |
| Distribution | One file per artifact, classpath-resolved | One file, self-contained |
| Reproducibility | Depends on classpath resolution | Fully captured in the file |
| License + audit | Manageable per-dep | Single file, harder to audit deps |
| Security patching | Bump one dep version | Rebuild uber |
| Bandwidth | Small artifact + classpath cache | Larger artifact, no cache |

Pick thin for everything that other projects might depend on; pick uber for everything you'd hand to a runtime.

---

## Resources

### The Java pattern (recommended)

`src/main/resources/` parallel to `src/main/cajeta/`. Resources keep
their original filenames + extensions; the directory structure
under `resources/` becomes the runtime lookup path. The compiler
bundles them into the output archive (or embeds them into the
binary for `--emit=exe`) at the same relative paths.

Runtime access via `cajeta.lang.Resources`:

```cajeta
package cajeta.lang;

public final class Resources {
    // Read a resource at the given archive-relative path as bytes.
    // Returns null if the resource doesn't exist in any loaded
    // archive (production resources searched first, then test if
    // running under test).
    public static byte[] loadBytes(String path);

    // Read a resource as a String, decoding bytes per the given
    // encoding (defaults to UTF-8).
    public static String loadText(String path,
                                   Encoding encoding = Encoding.UTF_8);

    // Stream a resource — for large files that shouldn't materialize
    // entirely in memory.
    public static InputStream open(String path);

    // List all resources under a prefix. Useful for plugin discovery
    // and similar "find every resource matching a pattern" cases.
    public static Iterable<String> list(String prefix);
}
```

Usage:

```cajeta
import cajeta.lang.Resources;

String html = Resources.loadText("templates/greeting.html");
byte[] modelWeights = Resources.loadBytes("ml/resnet50.safetensors");
for (path in Resources.list("config/")) {
    Config c = parseConfig(Resources.loadText(path));
    ...
}
```

### Why this approach

The Java pattern works in cajeta because it answers four questions
without contortion:

1. **Where do resources live in source?** Sibling directory to
   code. Easy to find, doesn't pollute the source tree.
2. **How do they end up in the artifact?** Compiler bundles them
   into the archive (or binary) at parallel paths.
3. **How does code address them?** A path-based API
   (`Resources.load(path)`) that works the same way in development
   (reading from `src/main/resources/`) and in production (reading
   from the linked-in blob).
4. **How are test-only resources kept out of production?** Separate
   directory hierarchy (`src/test/resources/`); test classloader
   sees both, production sees only `src/main/resources/`.

### Alternatives considered

- **Rust's `include_bytes!` macro.** Embed a single file at
  compile time, baked into the binary. Pro: zero runtime overhead.
  Con: doesn't scale past a handful of resources, no directory
  discovery, no test-vs-prod separation. Rust itself struggles
  with this — most large Rust projects use `rust-embed` or
  similar to provide the missing structure.
- **Go's `//go:embed` directive + `embed.FS`.** Declarative,
  filesystem-shaped runtime API. Cleaner than `include_bytes!`.
  Pro: discoverable. Con: requires special compiler directives at
  every embed site rather than a convention-based bundle.
- **Asset bundlers (webpack-style).** Resources compiled into
  hashed-filename bundles, served via a manifest. Pro: deduplication,
  content-addressed paths. Con: heavy infrastructure for what's
  usually a small need; better suited to web frontends than
  general-purpose languages.
- **Top-level `resources/` directory** (not under `src/main/`).
  Cleaner separation but breaks the Maven convention many cajeta
  users will be familiar with from other languages. Lean toward
  the Maven convention — sticking with established practice
  reduces friction for users coming from Java / Kotlin / Scala.

The Java pattern is the recommended path; the alternatives are
documented so future RFCs that revisit this decision have the
starting context.

### Resource compression

Resources bundled into the archive get the same per-entry zstd
compression as bitcode entries. Configuration knob: `cajeta.toml`'s
`[resources]` section can disable compression on a path pattern
for resources that are already compressed (PNG, MP4, model
weights, etc.) — compressing already-compressed data wastes time
and slightly bloats the output.

```toml
[resources]
no-compress = ["*.png", "*.jpg", "*.mp4", "*.safetensors", "*.gz", "*.zst"]
```

---

## Binary releases

### Supported targets (v1)

Tier 1 — production-supported, CI-tested, distributable artifacts:

| Triple                          | Description                              |
|---------------------------------|------------------------------------------|
| `x86_64-linux-gnu`              | Linux glibc on 64-bit Intel / AMD        |
| `x86_64-pc-windows-msvc`        | Windows on 64-bit Intel / AMD            |
| `x86_64-apple-darwin`           | macOS on Intel                           |
| `aarch64-apple-darwin`          | macOS on Apple Silicon                   |
| `aarch64-linux-gnu`             | Linux glibc on 64-bit ARM (Graviton, Pi) |
| `wasm32-wasi`                   | WebAssembly + WASI for server runtime    |

Tier 2 — buildable, lightly tested, no distributable binaries:

| Triple                          | Description                              |
|---------------------------------|------------------------------------------|
| `x86_64-pc-windows-gnu`         | Windows via MinGW                         |
| `x86_64-unknown-linux-musl`     | Static-linkable Linux musl                |
| `aarch64-linux-musl`            | Static-linkable ARM Linux                 |
| `wasm32-unknown-emscripten`     | WebAssembly + Emscripten (browser)        |

Tier 3 — designed-in, not implemented v1:

- `aarch64-linux-android` — Android
- `aarch64-apple-ios` — iOS
- `thumbv7em-none-eabihf` — Bare-metal ARM Cortex-M
- `riscv64-unknown-linux-gnu` — RISC-V Linux

Tier-3 targets need additional work: the C runtime (cajeta_runtime.c)
uses POSIX-only APIs (`pthread`, `getentropy`, `ucontext`) that
embedded and mobile targets need substitute implementations for.

### Triple syntax

Standard LLVM triple format: `<arch>-<vendor>-<os>-<env>`. The
cajeta compiler passes the triple straight through to LLVM's
TargetMachine, so any triple LLVM accepts works at the codegen
level — but only tier-1 targets are validated end-to-end.

The `--target` flag overrides the default (host triple); `--cpu`
and `--features` select the specific CPU model and feature flags
within the target (e.g. `--cpu=skylake --features=+avx2,+bmi2`).

### Linker integration

For `--emit=exe`:

- **lld in-process** (default when available). The compiler links
  to `lld` as a library and drives the link step in-process. No
  external linker invocation, no temp file shuffling, fast.
  Requires `lld-18-dev` at compile-build time; auto-detected by
  CMake (`CAJETA_HAS_LLD` flag).
- **System linker fallback.** When lld isn't available, the
  compiler writes the per-module object files to a temp directory
  and invokes the system linker (`ld` on Linux, `link.exe` on
  Windows, `ld` on macOS) via `execvp`. Slower (process startup +
  file I/O) but works on any system with a C toolchain.

For static binaries on Linux:

```
cajeta build --target=x86_64-unknown-linux-musl
```

Pulls musl-libc and statically links. The resulting binary runs
on any Linux kernel of sufficient version (≥3.2 for the runtime's
ucontext usage) with no glibc dependency.

For Windows + signed binaries:

```
cajeta build --target=x86_64-pc-windows-msvc \
             --sign-cert path/to/cert.pfx \
             --sign-password ...
```

(Signing is a post-link step the cajeta build tool wraps around the
compiler — not a compiler feature itself.)

---

## Optimization

### `-O` levels

Standard LLVM levels, plus cajeta-specific extensions:

| Flag       | LLVM equivalent | Description                                 |
|------------|-----------------|---------------------------------------------|
| `-O0`      | -O0             | No optimization. Fast compile, slow runtime, full debug info. Default for `--debug` builds. |
| `-O1`      | -O1             | Basic optimization. Drops obvious dead code, simple inlining. |
| `-O2`      | -O2             | Standard release. Inline + vectorize + most loop opts. Default for `--release` builds. |
| `-O3`      | -O3             | Aggressive inlining + vectorization. Larger code; sometimes slower than -O2 due to cache effects. |
| `-Os`      | -Os             | Size-optimize. -O2 with bias against transforms that grow code. |
| `-Oz`      | -Oz             | Minimal size. -Os with aggressive size-over-speed. |
| `-Ofast`   | -Ofast          | -O3 + relaxed floating-point semantics (fast-math, FTZ, denormals-to-zero). Unsafe for IEEE-strict code. |

### Link-time optimization (LTO)

LTO defers the optimizer to after all modules are linked together,
letting it inline across module boundaries and devirtualize through
the whole program. Big wins for stdlib + user-code call sites that
would otherwise stop at the archive boundary.

```
cajeta build -O2 --lto=thin       # ThinLTO — fastest LTO, scales well
cajeta build -O2 --lto=full       # Full LTO — best optimization, slow link
cajeta build -O2 --lto=off        # No LTO (default)
```

ThinLTO is the recommended choice for production builds — close to
full-LTO performance, much faster link times. Full LTO is
worthwhile only for the very-final-shipping release of a perf-
sensitive program.

### Profile-guided optimization (PGO)

Two-pass:

```
# Pass 1: instrumented build
cajeta build --pgo=instrument -o build/instrumented

# Run the instrumented binary against representative workloads;
# .profraw files get written to ./pgo-data/
./build/instrumented [args...]

# Pass 2: optimized build using collected profile
cajeta build --pgo=use=pgo-data/ -O2
```

PGO is worth ~5-15% on top of -O2 for typical workloads.
Particularly effective for the JIT-style dispatch cajeta does
(vtable lookups, fiber scheduling) — the optimizer learns hot
inline candidates and lays out hot branches favorably.

### Cajeta-specific IR passes

Run by the cajeta compiler before handing IR to LLVM (so LLVM's
own passes see already-cleaned IR):

- **Drop-chain elimination.** Local owners whose drop is never
  observable (the value doesn't escape, no exception path, drop
  fn is a no-op) skip the runtime drop_push / drop_pop_run pair.
- **Vtable devirtualization for sealed classes.** A call through
  a `sealed` class hierarchy with one implementer is rewritten as
  a direct call. LLVM does some of this on its own but the cajeta
  pass has the class-hierarchy info before LLVM sees the IR.
- **Structural-hash inlining.** Compiler-synthesized
  `Object.hash()` calls get the field-walk inlined at the call
  site when the receiver's runtime type is known. Cuts the call
  overhead for hash-flooded loops (HashMap probes, sorting, etc.).
- **Generic-instantiation cache.** Repeated instantiations of the
  same generic with the same type arguments share one LLVM module.
  Avoids combinatorial code bloat on programs with many
  `Box<T>` / `Optional<T>` style wrappers.

These are always on at `-O1` and above; disable individually via
`--pass-disable=<name>` for debugging compiler issues.

### IR-level efficiency knobs

- `--inline-threshold=<n>` — override LLVM's default inline-cost
  threshold. Higher value = more inlining (typically faster +
  bigger). Default tracks `-O` level.
- `--vectorize=on|off|loop|slp` — control LLVM auto-vectorization.
  Loop vectorizer + SLP (superword-level parallelism) both on at
  -O2+.
- `--unroll=on|off` — loop unrolling.
- `--debug-info=full|line|off` — DWARF emission. `full` includes
  variable info; `line` is just file:line mapping; `off` strips
  all debug info for smallest binaries.

### Tradeoffs to know

- **`-O3` vs `-O2` is not always faster.** -O3's extra inlining
  can blow out the instruction cache on hot loops. Benchmark
  before assuming -O3 wins.
- **LTO multiplies link time.** ThinLTO is ~2-3× a non-LTO link;
  full LTO is ~10-20×. Iteration speed during development drops
  meaningfully. Recommend LTO only for tagged releases.
- **PGO is workload-specific.** A profile collected on one
  workload (e.g. CPU-bound batch processing) misses the right
  inline targets for a different workload (e.g. latency-bound
  request handling). Collect profiles representative of the
  shipping workload, not arbitrary microbenchmarks.

---

## Compiler flag index

Alphabetical reference. Flags work for both the `cajeta` compiler
binary (low-level) and `cajeta build` / `cajeta test` (high-level
build-tool wrapper); the build tool's flavor flags (`--release`,
`--debug`) expand to combinations of compiler flags listed here.

### Source / output

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--source-root=<path>`        | Root of the source tree. Defaults to `src/main/cajeta`. |
| `--test-source-root=<path>`   | Test source tree root. Defaults to `src/test/cajeta`. |
| `--resource-root=<path>`      | Resources tree. Defaults to `src/main/resources`. |
| `--test-resource-root=<path>` | Test resources tree. Defaults to `src/test/resources`. |
| `--entry-method=<name>`       | Entry point (for `--emit=exe`). e.g. `com.example.Main::main`. |
| `--output=<path>`, `-o <path>`| Output path. Defaults vary by `--emit`.        |
| `--classpath=<paths>`         | Colon-separated (Unix) / semicolon-separated (Windows) list of archive paths the compiler resolves stdlib + library types against. |
| `--archive-out=<path>`        | Path for `--emit=archive` output. Defaults to `build/archive/<project>.cja`. |

### Emit mode

| Flag                | Description                                          |
|---------------------|------------------------------------------------------|
| `--emit=ir`         | Text LLVM IR (`.ll`). Default for development.       |
| `--emit=obj`        | Native object files (`.o` / `.obj`).                 |
| `--emit=archive`    | Cajeta archive (`.cja`). For library distribution.   |
| `--emit=exe`        | Linked native executable. Requires entry method.     |

### Target

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--target=<triple>`           | LLVM target triple. Default: host triple.      |
| `--cpu=<name>`                | CPU model within target (e.g. `skylake`). Default: `generic`. |
| `--features=<list>`           | Comma-separated CPU features (`+avx2,+bmi2`).  |

### Optimization

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `-O0`                         | No optimization.                               |
| `-O1`                         | Basic optimization.                            |
| `-O2`                         | Standard release optimization. Default for release builds. |
| `-O3`                         | Aggressive optimization.                       |
| `-Os`                         | Size-optimize.                                  |
| `-Oz`                         | Minimal size.                                  |
| `-Ofast`                      | -O3 + relaxed floating-point.                  |
| `--lto=off|thin|full`         | Link-time optimization. Default `off`.         |
| `--pgo=instrument|use=<dir>`  | Profile-guided optimization.                   |
| `--inline-threshold=<n>`      | LLVM inline cost threshold. Default per -O.    |
| `--vectorize=on|off|loop|slp` | Auto-vectorization control. Default per -O.    |
| `--unroll=on|off`             | Loop unrolling. Default per -O.                |
| `--pass-disable=<name>`       | Disable a specific cajeta or LLVM pass.        |

### Code generation

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--bounds=on|off`             | Array bounds-check generation. Default `on`.   |
| `--debug-info=full|line|off`  | DWARF debug info. Default `line`.              |
| `--frame-pointer=all|non-leaf|none` | Frame-pointer emission. Default `non-leaf`. |
| `--strip-symbols`             | Strip symbol table from output. Combine with `-O2`+ for release. |

### Diagnostics

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--warn=<list>`               | Comma-separated warnings to enable.            |
| `--warn-error=<list>`         | Promote listed warnings to errors.             |
| `--Werror`                    | Promote all warnings to errors.                |
| `--diagnostic-format=plain|json` | Diagnostic output format. Default `plain`.  |
| `-v`, `--verbose`             | Verbose output. Print phase timings.           |
| `--time-passes`               | Per-pass timing breakdown.                     |

### Build tool flavors (`cajeta build` / `cajeta test`)

| Flag             | Expands to                                                    |
|------------------|---------------------------------------------------------------|
| `--release`      | `-O2 --lto=thin --strip-symbols --debug-info=line`           |
| `--debug`        | `-O0 --debug-info=full --bounds=on`                          |
| `--debug-release`| `-O2 --debug-info=full --bounds=on` (release perf, debug info) |
| `--fast`         | `-O3 --lto=thin --debug-info=off` (max perf, no debug info)  |
| `--minimal`      | `-Oz --lto=full --strip-symbols --debug-info=off` (min size) |

### Archive operations (cajeta toolchain `car` command)

| Subcommand              | Description                                    |
|-------------------------|------------------------------------------------|
| `cja create <path>`     | Create a new archive from a directory.         |
| `cja ls <archive>`      | List archive contents.                          |
| `cja cat <archive> <path>` | Read one entry to stdout.                    |
| `cja extract <archive> --out=<dir>` | Extract entries to a directory.    |
| `cja verify <archive>`  | Verify checksums + format integrity.            |
| `cja info <archive>`    | Print archive manifest + summary stats.         |

---

## Open questions

- **Should the build tool be a separate binary or part of the
  compiler?** Rust splits `rustc` (compiler) from `cargo` (build
  tool). Java keeps `javac` separate from `maven` / `gradle`
  (third-party). Cajeta could go either way. Lean: separate
  binary (`cajeta` build tool wrapping `cajetac` compiler) — keeps
  the compiler focused on translating source to artifacts and
  lets the build tool evolve dependency / project management
  independently.
- **Format-version compatibility window.** How many `.cja` format
  versions back should the compiler accept? Rust's rlib format
  effectively requires matched compiler version; Java's `.class`
  format supports decades-back class files. Lean: support N-2 (the
  two prior format versions) and document the deprecation
  window per release.
- **Distributed archive registry.** Some way for cajeta users to
  share archives without manual `lib/` vendoring. A simple HTTP
  registry (npm-style) is the obvious answer but commits to
  significant infrastructure. Defer to a separate `cajeta-registry`
  RFC.
- **Reproducible builds.** Same source + same compiler + same flags
  → byte-identical archive output. Build timestamps in the
  manifest break this; offer `--source-date-epoch` (Reproducible
  Builds standard) to pin the timestamp from an env var.
- **Cross-archive symbol versioning.** When a stdlib archive's
  method signature changes, every user archive referencing it
  needs to be rebuilt or it crashes at link/load time. Solutions
  range from "ignore the problem, recompile everything" to "ABI
  version per archive entry, compatibility checks at load." Lean
  toward the simple end for v1 and revisit when real-world
  ecosystems make the problem painful.
