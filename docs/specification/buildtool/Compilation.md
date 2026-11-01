# Compilation

Specification for how cajeta source becomes a runnable program. It
covers the source-tree layout the compiler expects, the phases the compiler
runs, the artifacts it produces, the flags that control each step,
and the binary / archive targets supported.

This document is the contract between the cajeta toolchain and its
users. Behaviors named here are stable across point releases.
Design docs in `docs/` (stdlib/* and
specification/reflect/Reflection.md, etc.) describe what the
content of those artifacts is. This one describes the machinery.

> **Implemented vs. planned.** Parts of this spec describe the
> design target rather than the current binary. Where the two
> diverge this doc says so inline ("planned" / "not yet wired").
> The authoritative surface is what `cajeta --help` prints (parsed
> in `src/main.cpp`), and the archive container is
> `src/cajeta/compile/CajetaArchive.{h,cpp}`. As of this writing
> the compiler binary takes three positional arguments, `cajeta
> [options] <entry-method> <source-root> <archive-root>`. The
> per-root flags some older drafts of the flag index implied do
> not exist. See
> [Compiler flag index](#compiler-flag-index) for the verified set.

## Table of contents

1. [Source tree structure](#source-tree-structure)
2. [Compilation phases](#compilation-phases)
3. [Output formats](#output-formats)
4. [Archive format](#archive-format)
5. [Uber archives](#uber-archives)
6. [Classpath ingestion](#classpath-ingestion)
7. [Resources](#resources)
8. [Binary releases](#binary-releases)
9. [Optimization](#optimization)
10. [Compiler flag index](#compiler-flag-index)

---

## Source tree structure

Project layout, Maven-style with cajeta naming:

```
<project>/
├── cajeta.json                 # project manifest (JSONC: name, version,
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
    ├── ir/                     # --emit=ir output (exploded)
    ├── obj/                    # --emit=obj output (exploded)
    ├── exe/                    # --emit=exe output
    ├── cja/                    # --emit=cja output (project-only archive)
    └── uber/                   # --emit=uber output (self-contained archive)
```

**`src/main/cajeta/` and `src/test/cajeta/`.** Package directories
mirror the canonical name. `com.example.User` lives at
`src/main/cajeta/com/example/User.cajeta`. The compiler's
package-path check (`onPackageDeclaration`) enforces this.

**`src/main/resources/` and `src/test/resources/`.** These are
bundled into the output archive at parallel paths, or linked into
the binary as embedded blobs for `--emit=exe`. Test resources are
accessible only to test code, and production resources are
accessible to both.
See [Resources](#resources) for the runtime API.

**`cajeta.json`.** This is the project manifest (JSONC). It names
the project, declares version + cajeta language version, lists
archive dependencies (local paths or registry references), pins
default target triples, and configures build flavors (release,
debug, fast). It is driven by the `cajeta` build tool (`cajeta
build`, `cajeta test`, etc.), which wraps the compiler. The full
schema is in [`BuildTool.md`](BuildTool.md#manifest--cajetajson).

**`lib/`.** This holds local archive dependencies. The cajeta
compiler's `--classpath` flag points at this directory or at
specific archives within it. User code references types defined in
those archives by canonical name, resolved at compile time against
the archives' embedded class metadata.

The Maven `src/main/<lang>` convention exists for reasons that
apply to cajeta too. It cleanly separates production from test, it
gives resources their own first-class root with no scattered
`.txt` files mixed with source, and it survives multi-language
projects, so a future `src/main/c/` for FFI shims wouldn't break
anything.

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

**Phase 1, Parse.** `.cajeta` files in `src/main/cajeta/` (or the
explicit source root) are walked, lexed, and parsed. Cross-file
forward references are recorded as placeholders. The archive
pre-scan registers every declared class name before any module
parses, so a class referenced before its declaration parses works.

**Phase 2, Resolve.** The class registry (canonicalMap) is
populated. Template instantiations are created on demand. Deferred
prototypes build to fixed point. Classes whose superclass was a
placeholder at visit time get their layout deferred until the
parent's prototype lands. The post-parse sweep walks until no class can
make further progress.

**Phase 3, Lower.** Each method becomes an LLVM function. Class
layouts become LLVM aggregate types. Vtable globals + RTTI globals
get emitted. Cross-module references, such as calls to stdlib
methods and references to stdlib vtables, go through
`ensureFunctionInModule` / `ensureGlobalInModule` to insert extern
declarations in the calling module. The merge step resolves them
to definitions when the archive's bitcode joins user IR.

**Phase 4, Optimize.** LLVM passes per `-O` level. The cajeta
compiler runs the standard LLVM pass pipeline plus a few cajeta-
specific passes (drop-chain elimination, vtable devirtualization
for sealed classes, structural-hash inlining). LTO and PGO are
opt-in via flags.

**Phase 5, Emit.** The output format is set by `--emit`:

- `--emit=ir` writes exploded text LLVM IR (`.ll`), one file per module under a directory tree mirroring the source-tree package layout.
- `--emit=obj` writes exploded native objects (`.o` / `.obj`) in the same directory shape.
- `--emit=cja` writes a single cajeta archive (`.cja`) bundling only the user project's compiled bitcode + a manifest. It holds no stdlib and no dependency archives. It is the library distribution form, and consumers bring their own stdlib and dependency archives via `--classpath`. See [Archive format](#archive-format).
- `--emit=uber` writes a single cajeta archive (`.cja`) bundling the user's modules, the parsed stdlib, AND every dependency archive on the `--classpath` that user code transitively references. Dep entries are nested under `deps/<name>-<version>/` so each contributing archive is identifiable in the bundle. It is Java's "uber-jar" equivalent. It uses the same `.cja` container, and the manifest's `kind` field distinguishes the two. See [Uber archives](#uber-archives).
- `--emit=exe` writes a linked executable for the native target.

---

## Output formats

### `--emit=ir` (exploded text LLVM IR)

Human-readable LLVM `.ll` output per cajeta module, laid out as a directory tree mirroring the source-tree package layout. One `.ll` file per `.cajeta` source plus one for the stdlib (`cajeta.runtime.__stdlib__.ll`). It is useful for debugging the compiler, inspecting generated IR, and feeding into external LLVM tooling (`opt`, `llc`, `llvm-link`). It is not intended for deployment.

```
build/ir/
├── cajeta.runtime.__stdlib__.ll       ← stdlib + embedded C runtime
└── <package>/<sub>/<Class>.ll         ← one per user source
```

### `--emit=obj` (exploded native object)

Native object files per module, meaning ELF `.o` on Linux, COFF
`.obj` on Windows, and Mach-O on macOS. The directory shape is the
same as `--emit=ir`. The caller links with a system linker. This
is useful when integrating cajeta code into a larger build that
already has its own linker invocation.

### `--emit=cja` (cajeta archive `.cja`, project-only)

A single archive file containing only the user project's compiled bitcode plus a manifest. The parsed stdlib bundle is stripped, and no dependency archives are absorbed. Consumers bring their own stdlib and deps via `--classpath` at compile time. It is the distribution format for cajeta libraries, and the stdlib itself ships as a `.cja` of this form.

The manifest's `kind` field is `"cja"`. The `deps` array is absent, because a cja archive never carries deps. If you want a self-contained bundle, use `--emit=uber`.

See [Archive format](#archive-format) for the on-disk shape.

### `--emit=uber` (cajeta archive `.cja`, uber)

The same `.cja` container as `--emit=cja`, but it bundles user modules plus the parsed stdlib plus every dependency archive on `--classpath` that user code transitively references. This is Java's "uber-jar" pattern. User code lives at the archive's top level, and dep entries live nested under `deps/<name>-<version>/<original-path>`. The manifest's `kind` field is `"uber"` and its `deps` array lists each contributing dep (name, version, included entry count).

It is used for deployment, as one self-contained `.cja` you can hand to a runner without separately distributing the classpath. It adds storage size, since the stdlib + every transitive dep is inlined. It is not appropriate for libraries, which want others to consume them as deps and never absorb them whole.

See [Uber archives](#uber-archives) for the nested layout, manifest schema, and reachability rules.

### `--emit=exe` (linked executable)

A native executable for the configured target triple. It links the
user's compiled modules with the cajeta stdlib archive, the C
runtime, and any third-party archives on the classpath into a
single binary using the in-process `lld`, or the system linker if
lld isn't available. Embedded resources get baked into the
executable as `.rodata` segments addressable by path at runtime.

---

## Archive format

Cajeta archives use a custom container format with the file
extension `.cja` (Cajeta ARchive). The design priority is
write-few-read-many. A stdlib archive gets built once and
loaded by every cajeta program that runs against it, so
decompression speed and seekable random access matter more than
compression-time throughput.

### Compression algorithm

The algorithm is zstd (Zstandard), at levels 19-22 for archive
builds and level 3 for incremental development builds.

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
one class's bitcode without touching the rest of the archive. That
is exactly what cajeta needs, since a compiler reading a stdlib
archive typically wants ~3-5 classes per user file and never the
whole hierarchy.

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
│   what the compiler writes today (v1, minimal): │
│   {                                             │
│     "name": "cajeta-stdlib",                    │
│     "version": "1.0.0",                         │
│     "kind": "cja",                              │
│     "format_version": 1,                        │
│     "entry_count": 87                           │
│   }                                             │
│   (uber archives add a "deps" array; the richer │
│    cajeta_lang_version / target_triple /        │
│    build_timestamp / build_flavor fields are    │
│    the design target, NOT yet emitted — readers │
│    like `cajeta archive info` already scan for   │
│    them and print blanks when absent.)          │
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

The index sits at the end of the file so an archive can be written
in a single forward pass. The writer writes entries as they're
compressed, then writes the index, then patches the header's
`index_offset` field. Readers seek to the end, read the index,
then random-access individual entries.

Format-version bumps are reserved for incompatible changes.
Backward-compatible additions (new entry kinds, new manifest
fields) don't bump the version. The compiler refuses to load an archive whose
format version exceeds its supported maximum.

### Minimum-viable v1

The full container above (zstd, trailing index, resources, runtime-
bitcode block, separate manifest blob) is the design target. The
v1 implementation lands a partial subset so each remaining piece
can ship incrementally without blocking on the others:

- **Compression: zstd (level 3) on both manifest and entries by default.** Header flag bits 0 (`manifest_compressed`) and 1 (`entries_compressed`) flip when compression is on. Each compressed section is framed as `uint64 uncompressed_length || zstd_bytes`. The manifest_length / data_length values give the ON-DISK size, and the uint64 prefix gives what to allocate before decompressing. The writer opts out via `CajetaArchive::setCompression(Compression::None)`, and the reader handles either path. CLI plumbing (`--archive-compress=on|off`) is a thin follow-up. The size win is that HelloWorld's `.cja` goes from 418 KB → 173 KB (60% off) on stdlib-heavy content.
- **Trailing random-access index.** Every archive writes it, and readers can use it (via `header.index_offset != 0`) or skip it and scan sequentially. The format is `uint32 entry_count` followed by per-entry `(uint32 name_length, name bytes, uint64 entry_offset, uint64 entry_on_disk_size)`. `CajetaArchive::findEntry(name)` exposes O(1) lookup on the read side. The original spec's JSON-shape index sketched out in the design doc is reserved for a richer v2 (compressed_size, uncompressed_size, checksum_xxh3, kind metadata). v1's compact binary index is sufficient for the random-access use case.
- **No resources block, no runtime-bitcode block.** v1 has only class bitcode entries. Resources land alongside the `@Embedded` annotation work. The runtime bitcode block lands when `--emit=exe` migrates from the inline-stdlib-module shape to consuming a stdlib `.cja`.
- **Single-file output.** The writer produces one `.cja` per invocation. Multi-archive output (`--emit=cja --split` for per-package archives) is not on the v1 roadmap.

The header shape, magic bytes, format version field, manifest schema, and entry encoding ARE final in v1. Additions land via flag bits and manifest fields, and never via format-version bumps.

### Entry encoding (v1)

Each entry is length-prefixed and self-describing:

```
┌──────────────────────────────────────────────────────────────┐
│ name_length  (4 bytes, uint32 LE)                            │
│ name         (name_length bytes, UTF-8)                      │   e.g. "demo/App.bc"
│ origin_tag   (1 byte)                                        │   0=user, 1=stdlib, 2=dep
│ kind_tag     (1 byte)                                        │   0=class bitcode, 1=resource, 2=runtime bitcode, 3=class source
│ reserved     (2 bytes)                                       │   zero
│ data_length  (8 bytes, uint64 LE)                            │
│ data         (data_length bytes)                             │   raw bytes (LLVM bitcode for kinds 0/2, raw file for kind 1, UTF-8 .cajeta source for kind 3)
└──────────────────────────────────────────────────────────────┘
```

Names use forward-slash path separators regardless of host platform, which matches the `.jar` / `.zip` convention. Bitcode entries use the `.bc` extension, class-source entries use the `.cajeta` extension, and resources keep their original extension.

`origin_tag` lets readers bucket each entry without parsing the manifest. The values are 0=user, 1=stdlib (uber only, since cja strips stdlib), and 2=dep (uber only). In uber archives, the `deps/<name>-<version>/` path prefix carries the symbolic name, and the tag is the categorical bucket. Cja archives only contain entries with origin_tag=0.

`kind_tag=3` (ClassSource) carries the original `.cajeta` source bytes of each user class. That is the substrate the [classpath ingestion](#classpath-ingestion) machinery re-parses at the next compile. Cja and uber archives both ship one `ClassSource` per user class, named `<canonical-slashed>.cajeta` (e.g. `deplib/Util.cajeta`). The stdlib bundle in uber archives has no single source file, since the parsed stdlib is many embedded files, so no `ClassSource` is emitted for it.

### Internal structure mirrors the source tree

For a project with package `com.example`:

```
src/main/cajeta/com/example/User.cajeta    →  com/example/User.bc
                                              com/example/User.cajeta    (ClassSource entry)
src/main/cajeta/com/example/Service.cajeta →  com/example/Service.bc
                                              com/example/Service.cajeta (ClassSource entry)
src/main/resources/templates/greet.html    →  templates/greet.html
src/main/resources/config/default.yaml     →  config/default.yaml
```

The path structure is the same, minus the `src/main/cajeta/` or
`src/main/resources/` prefix. Each user class lands as two
entries, the compiled `.bc` and the original `.cajeta` source for
classpath ingestion at the next compile. Resources keep their
original filename + extension.

This mirroring is intentional. A developer reading the archive (via
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

Archive inspection / repackaging is reached through the `cajeta
archive` subcommand. There is no separate `cja`/`car` binary, and
no `create` subcommand, because archives are produced by the
compiler's `--emit=cja|uber`:

```
cajeta archive list    <archive>
cajeta archive cat     <archive> <entry-path>
cajeta archive extract <archive> [-C <dir>]
cajeta archive verify  <archive>          # structural integrity + checksums
cajeta archive info    <archive>          # print manifest fields
cajeta archive deps    <archive>          # uber dep list
cajeta archive diff | repack | strip | merge | sign | verify-sig
```

The cajeta compiler embeds the archive writer, and the
read/transform subcommands live in
`src/cajeta/cli/ArchiveCommands.cpp`. The full reference is
[`ArchiveManagement.md`](ArchiveManagement.md).

---

## Uber archives

A `.cja` file produced by `--emit=uber` carries the user's modules, the parsed stdlib, AND every dependency archive on the `--classpath` that user code transitively references. It is the same container as a cja archive. The manifest's `"kind": "uber"` field and `"deps"` array, together with the `deps/<name>-<version>/` path prefix on every dep entry, let consumers distinguish bundled deps from user code.

### Why uber

Java's `.jar` ecosystem has the same split. Thin jars are library archives. They depend on other libraries at compile time and get assembled into a complete classpath at deployment time. Uber jars (also "fat jars" or "shaded jars") collapse a tree of dependencies into a single self-contained artifact. You hand the file to whoever's running it, they invoke a runner, and you are done. There is no classpath wrangling and no version mismatch between user code and bundled deps.

Cajeta inherits the same rationale. Libraries publish as project-only `.cja`s (`stdlib.cja`, `my-json-utils.cja`, …) so consumers can pin / replace specific versions. Applications publish as uber `.cja`s so deployment is one file. Microservices, CLI tools, sample programs, and demos are all natural fits for uber.

### Why nested `deps/<name>-<version>/`

Earlier drafts of this spec flattened every dep entry to the archive's top level alongside user code, with a per-entry `origins` map in the manifest reporting which dep produced each path. Switching to a nested layout, where each dep gets its own `deps/<name>-<version>/` subtree, buys three things:

- **Faster reads.** A consumer that needs everything from one dep (e.g. LTO across a whole dep's bitcode) gets a contiguous range query rather than scattered per-entry index lookups.
- **Cleaner pruning.** When the reachability closure drops every entry from a dep, the whole subtree disappears AND that dep's `deps` array entry drops. The manifest reflects what's actually bundled, with no per-entry bookkeeping.
- **Provenance you can `ls`.** `cja ls my-app-uber.cja` shows you the dep tree directly. There is no "look at this entry, then cross-reference the origins map" indirection.

Same-class collisions across deps stop being a writer-level concern, since each dep has its own subtree namespace. They become a version-collision concern instead, surfaced via the manifest's `deps` array.

### Manifest extensions

A cja manifest:

```json
{
    "name": "my-lib",
    "version": "1.0.0",
    "kind": "cja",
    "cajeta_lang_version": "1.0",
    "dependencies": [
        { "name": "cajeta-stdlib", "version": "1.0.0" },
        { "name": "json-utils",     "version": "2.1.4" }
    ],
    "entry_method": "demo.App.run"
}
```

The `dependencies` array on a cja archive is metadata describing what the project was compiled against. The resolver re-reads it at consumption time to assemble the classpath. The cja archive does not itself contain those deps.

An uber manifest replaces `dependencies` with a richer `deps` array describing what's actually bundled inside:

```json
{
    "name": "my-app",
    "version": "1.0.0",
    "kind": "uber",
    "cajeta_lang_version": "1.0",
    "entry_method": "demo.App.run",
    "deps": [
        { "name": "cajeta-stdlib", "version": "1.0.0", "included_entry_count":  85 },
        { "name": "json-utils",     "version": "2.1.4", "included_entry_count":   7 }
    ]
}
```

The `deps` array is exhaustive. Only deps that contributed at least one surviving entry after pruning land here. Each entry's bitcode lives under `deps/<name>-<version>/<original-path>` in the archive. Name + version are the substring you can search for, and the consumer can reconstruct the original dep set without parsing per-entry metadata. A `content_checksum` field per dep for reproducible-build verification is a future addition.

### Inclusion rules

Only transitively referenced dependency classes get bundled. The compiler runs a reachability analysis from the entry method and the user-code roots, and classes that nothing references are excluded. This keeps uber archives lean, so bundling `cajeta-stdlib` doesn't drag in JSON, hashing, parallel streams, etc. if user code doesn't use them.

The reachability set includes:

1. The `entry_method` and everything it transitively calls.
2. Every class user code declares a field, parameter, return type, or local of.
3. Every class user code's body references by name (including reflective uses via `@Reflect` annotation, see Reflection.md).
4. Anything those classes recursively reference, transitively to fixed point.

Resources (`@Embedded`-annotated files, classpath-loaded YAMLs, etc.) are bundled separately, and a `resources` flag on `--emit=uber` controls inclusion. The default includes resources from the user project and excludes those from deps. Deps include their own at consumption time via their own cja archive, and uber doesn't re-bundle them.

### Reachability — bitcode substring closure

The full reachability above wants the compile-time type graph (every CajetaClass field type, parameter type, return type, body reference). With [classpath ingestion](#classpath-ingestion) wired through parse + codegen, every user reference to a classpath class lands in the user bitcode as a mangled symbol, so the substring-scan closure over bitcode bytes faithfully captures the dependency set.

**Algorithm:**

1. **Seed.** Every user + stdlib module's staged bitcode is included by default.
2. **Iteration.** For each not-yet-included dep entry with canonical name `C`, scan every currently-included entry's bitcode bytes for the substring `C`. If matched, mark the dep included.
3. **Repeat** to fixed point. Including a dep may unlock OTHER deps whose canonical only appears in the just-promoted dep's bitcode (transitive reach).

**Why substring works.** Cajeta's bitcode embeds each class's canonical name in multiple places, namely the RTTI globals (`@"<canonical>#RttiGlobal"` carrying the literal C string `"<canonical>\0"`), the vtable globals (`@"<canonical>#VTable"`), and every method's mangled function name (`@"<canonical>::method(...)"`). Any reference to a class from another class's bitcode produces at least one of these markers. Substring is conservative. False positives bundle extra, which is acceptable, and false negatives drop needed entries, which is rare and would surface as a runtime missing-symbol.

**Override.** `--prune-uber=off` disables the closure and bundles every classpath entry. It is useful when:

- Reflective dispatch loads classes by string-encoded names the bitcode scan can't see.
- A dep's class is referenced only via a `@Native` C function whose name doesn't carry the canonical.
- Build determinism is more important than archive size (the closure depends on what user code happens to reference; opt-out gives bit-for-bit reproducible bundles).

### Versioning + collisions

Two deps with the same `name+version` are deduped at the path level. Both target the same `deps/<name>-<version>/` subtree, and per-path first-archive-wins keeps a single entry per nested path.

Two deps with the same name but different versions occupy *different* subtrees (`deps/foo-1.0.0/` vs `deps/foo-2.0.0/`) and coexist physically. If the user's bitcode reaches into both versions of the same class, say it imports `foo.Util` and the resolver couldn't pin which one, that's a compile-time error long before the uber writer runs.

If `cja shade` (the namespace-renaming tool, planned) is used to repackage a dep, the renamed subtree gets the shaded name in the `deps` array, surfaced to the consumer.

### On-disk layout

It is the same container as a cja archive, just larger and with the manifest extensions above. A reader doesn't need to know it's an uber until it reads the manifest's `kind` field. cja and uber readers are interchangeable for the actual bitcode extraction.

```
my-app-uber.cja:
├── header  (CAJETA01 magic + version + flags + kind=1 (uber))
├── manifest (zstd-compressed JSON; "kind": "uber" + "deps" array)
│
├── demo/App.bc                                      ← user code (top level)
├── cajeta/runtime/__stdlib__.bc                     ← bundled stdlib (top level)
│
├── deps/cajeta-stdlib-1.0.0/cajeta/lang/Object.bc   ← nested per-dep subtree
├── deps/cajeta-stdlib-1.0.0/cajeta/lang/String.bc
├── deps/json-utils-2.1.4/json/JsonReader.bc
├── deps/json-utils-2.1.4/json/JsonWriter.bc
│
├── resources/                                       ← user-project only
│   └── templates/greet.html
│
└── index (zstd-compressed JSON pointing at all the above)
```

### Trade-offs

| | Cja | Uber |
|---|---|---|
| Use case | Library | Application |
| File size | Small (user code only) | Large (user + stdlib + deps) |
| Distribution | One file per artifact, classpath-resolved | One file, self-contained |
| Reproducibility | Depends on classpath resolution | Fully captured in the file |
| License + audit | Manageable per-dep | Single file, `deps` array enumerates bundled deps |
| Security patching | Bump one dep version | Rebuild uber |
| Bandwidth | Small artifact + classpath cache | Larger artifact, no cache |

Pick cja for everything that other projects might depend on, and pick uber for everything you'd hand to a runtime.

---

## Classpath ingestion

When the compiler runs with `--classpath=a.cja,b.cja,…`, each listed archive is loaded at compile-start and its classes are pulled into the consumer's compile so user code can `import` and reference them like any other class. This is what makes the cja/uber split useful. A library author publishes `mylib.cja`, the consumer adds it to `--classpath`, and `mylib`'s public surface is available during the consumer's parse + codegen.

### Mechanism

Cajeta does not parse LLVM bitcode to recover class metadata. Instead, every cja and uber archive ships the original `.cajeta` source bytes for each user class alongside the compiled `.bc`, as a separate entry tagged `EntryKind::ClassSource` (`kind_tag=3`). At ingestion time the compiler re-runs the standard parser pipeline against those source bytes, with the same parser, visitor, and prototype-builder it uses for the project's own sources. The result is a real `CajetaClass` registered in the canonical-name map with full method/field metadata, indistinguishable to the rest of the compiler from a class declared in the current project's tree.

Two phases run for the classpath, in order, before any user-source prescan:

1. **Prescan.** For every `ClassSource` entry in every classpath archive, the lexer/parser does a name-only walk and registers each class's canonical name in the archive registry. Forward references between classpath archives (`otherlib.X` declared in archive A, referenced by archive B) resolve to placeholders here that the next phase fills in.
2. **Parse.** Each `ClassSource` entry is parsed in full into a fresh "external" `CajetaModule`, with the module's `QualifiedName` derived from the entry's path (`deplib/Util.cajeta` → `deplib.Util`). The module's classes register in `canonicalMap`, the module's methods register in `Method::getArchive()`, and the module's LLVM module gets RTTI/vtable/method-prototype globals via `CajetaModule::buildPendingPrototypes()`.

After both phases complete, user-source prescan runs, then user-source parse. At that point the user's `import deplib.Util;` and any call like `Util.ten()` resolve to the classpath-loaded `CajetaClass` and `Method`, and codegen emits the standard cross-module extern declaration for the call site. That is the same machinery that resolves stdlib method calls in user IR.

### Why source-shipping, not metadata-only

There are three options for "tell the consumer compile what this archive exports":

- **Ship a class-metadata header** alongside each `.bc` (fields, methods, signatures, hierarchy, which is Java's `.class` model). The pros are a smaller, faster load with source not exposed. The cons are a separate format to design + maintain, and a separate metadata loader code path that has to track the parser semantically.
- **Parse the LLVM bitcode** to recover class shape. The pro is no archive-format addition. The con is that LLVM bitcode doesn't carry source-level intent (visibility, modifiers, template parameters). It would require structural reverse-engineering of cajeta's encoder conventions, and any encoder change breaks every consumer.
- **Ship the source** and re-parse. The pro is zero new code paths. The parser is the metadata loader, so the schema the consumer reads matches the schema the writer emitted by construction. The cons are source visibility, which is a real concern for proprietary libs, and archive size bloat.

The MVP ships source. It trades a small archive-size increase and a source-visibility concession for code-path simplicity. A metadata-only emit mode is a sensible follow-up for closed-source distribution.

### Output filtering

Classpath-ingested modules live in the compiler's `externalModules` list and stay out of the project's `modules` list. The emitter walks only `modules`, so classpath classes never get re-emitted into the consumer's `.cja` / `.ll` / `.o` / `.cja-uber` output. They remain external by definition. The classpath archive's own `.bc` entries are bundled in uber-form output via the existing dep-bundling path (under `deps/<name>-<version>/`), and the external-module pipeline plays no part in that.

### Stdlib

Today the cajeta compiler embeds the stdlib source files (`runtime/src/cajeta/`) into its own binary and parses them on every compile. The stdlib is not consumed via `--classpath`. A future shipping mode where the stdlib is a separate `cajeta-stdlib.cja` consumed via classpath uses exactly the same machinery. It's a packaging change, and the compiler design is unchanged.

### Diagnostics

When a classpath archive has no `ClassSource` entries (older `.cja` files predating this feature, or archives produced by a future metadata-only emit mode), `ingestClasspath` silently skips it. The bitcode is still bundled by uber emit, but the consumer's parser will see unresolved imports. A future cleanup will distinguish these cases explicitly.

When `CajetaArchive::readFrom` fails on a classpath path (file missing, format mismatch), the compiler prints the path + reason to stderr and aborts the compile. The alternative of silently degrading to "no classpath" would mask configuration bugs.

---

## Resources

### The Java pattern (recommended)

`src/main/resources/` sits parallel to `src/main/cajeta/`.
Resources keep their original filenames + extensions, and the
directory structure under `resources/` becomes the runtime lookup
path. The compiler
bundles them into the output archive (or embeds them into the
binary for `--emit=exe`) at the same relative paths.

Runtime access is via `cajeta.lang.Resources`:

```cajeta
package cajeta.lang;

public final class Resources {
    // Read a resource at the given archive-relative path as bytes.
    // Returns null if the resource doesn't exist in any loaded
    // archive (production resources searched first, then test if
    // running under test).
    public static int8[] loadBytes(String path);

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
int8[] modelWeights = Resources.loadBytes("ml/resnet50.safetensors");
for (path in Resources.list("config/")) {
    Config c = parseConfig(Resources.loadText(path));
    ...
}
```

### Why this approach

The Java pattern works in cajeta because it answers four questions
without contortion:

1. **Where do resources live in source?** In a sibling directory
   to code. It is easy to find and doesn't pollute the source
   tree.
2. **How do they end up in the artifact?** The compiler bundles them
   into the archive (or binary) at parallel paths.
3. **How does code address them?** Through a path-based API
   (`Resources.load(path)`) that works the same way in
   development, reading from `src/main/resources/`, and in
   production, reading from the linked-in blob.
4. **How are test-only resources kept out of production?** By a
   separate directory hierarchy (`src/test/resources/`). The test
   classloader sees both, and production sees only
   `src/main/resources/`.

### Alternatives considered

- **Rust's `include_bytes!` macro.** It embeds a single file at
  compile time, baked into the binary. The pro is zero runtime
  overhead. The con is that it doesn't scale past a handful of
  resources, has no directory discovery, and has no test-vs-prod
  separation. Rust itself struggles with this, and most large Rust
  projects use `rust-embed` or similar to provide the missing
  structure.
- **Go's `//go:embed` directive + `embed.FS`.** It is a
  declarative, filesystem-shaped runtime API, and it is cleaner
  than `include_bytes!`. The pro is that it is discoverable. The
  con is that it requires special compiler directives at every
  embed site rather than a convention-based bundle.
- **Asset bundlers (webpack-style).** Resources are compiled into
  hashed-filename bundles, served via a manifest. The pros are
  deduplication and content-addressed paths. The con is heavy
  infrastructure for what's usually a small need, and it is better
  suited to web frontends than general-purpose languages.
- **Top-level `resources/` directory** (outside `src/main/`). It
  is a cleaner separation but breaks the Maven convention many
  cajeta users will be familiar with from other languages. The
  lean is toward the Maven convention, because sticking with
  established practice reduces friction for users coming from Java
  / Kotlin / Scala.

The Java pattern is the recommended path. The alternatives are
documented so future RFCs that revisit this decision have the
starting context.

### Resource compression

Resources bundled into the archive get the same per-entry zstd
compression as bitcode entries. A planned `cajeta.json` `settings`
knob disables compression on a path pattern for resources that are
already compressed (PNG, MP4, model weights, etc.), because
compressing already-compressed data wastes time and slightly
bloats the output.

```jsonc
"settings": {
    "resources": {
        "no-compress": ["*.png", "*.jpg", "*.mp4", "*.safetensors", "*.gz", "*.zst"]
    }
}
```

---

## Binary releases

### Supported targets (v1)

Tier 1 is production-supported and CI-tested, with distributable artifacts:

| Triple                          | Description                              |
|---------------------------------|------------------------------------------|
| `x86_64-linux-gnu`              | Linux glibc on 64-bit Intel / AMD        |
| `x86_64-pc-windows-msvc`        | Windows on 64-bit Intel / AMD            |
| `x86_64-apple-darwin`           | macOS on Intel                           |
| `aarch64-apple-darwin`          | macOS on Apple Silicon                   |
| `aarch64-linux-gnu`             | Linux glibc on 64-bit ARM (Graviton, Pi) |
| `wasm32-wasi`                   | WebAssembly + WASI for server runtime    |

Tier 2 is buildable and lightly tested, with no distributable binaries:

| Triple                          | Description                              |
|---------------------------------|------------------------------------------|
| `x86_64-pc-windows-gnu`         | Windows via MinGW                         |
| `x86_64-unknown-linux-musl`     | Static-linkable Linux musl                |
| `aarch64-linux-musl`            | Static-linkable ARM Linux                 |
| `wasm32-unknown-emscripten`     | WebAssembly + Emscripten (browser)        |

Tier 3 is designed-in and not implemented in v1:

- `aarch64-linux-android` — Android
- `aarch64-apple-ios` — iOS
- `thumbv7em-none-eabihf` — Bare-metal ARM Cortex-M
- `riscv64-unknown-linux-gnu` — RISC-V Linux

Tier-3 targets need additional work. The C runtime
(cajeta_runtime.c) uses POSIX-only APIs (`pthread`, `getentropy`, `ucontext`) that
embedded and mobile targets need substitute implementations for.

### Triple syntax

The standard LLVM triple format is `<arch>-<vendor>-<os>-<env>`.
The cajeta compiler passes the triple straight through to LLVM's
TargetMachine, so any triple LLVM accepts works at the codegen
level. Only tier-1 targets are validated end-to-end.

The `--target` flag overrides the default (host triple). `--cpu`
and `--features` select the specific CPU model and feature flags
within the target (e.g. `--cpu=skylake --features=+avx2,+bmi2`).
`--cpu=native` resolves to the host's CPU model plus its detected
feature set. It is the equivalent of clang/gcc `-march=native`,
and it lets the optimizer's vectorizers use the host's widest ISA
(AVX2/AVX-512/FMA, NEON, …) instead of the conservative `generic`
baseline.

### Linker integration

For `--emit=exe`:

- **lld in-process** (default when available). The compiler links
  to `lld` as a library and drives the link step in-process. There
  is no external linker invocation and no temp file shuffling, so
  it is fast. It requires `lld-<LLVM-major>-dev` at compile-build
  time (e.g. `lld-23-dev`), and CMake auto-detects it
  (`CAJETA_HAS_LLD` flag).
- **System linker fallback.** When lld isn't available, the
  compiler writes the per-module object files to a temp directory
  and invokes the system linker (`ld` on Linux, `link.exe` on
  Windows, `ld` on macOS) via `execvp`. It is slower, due to
  process startup + file I/O, but it works on any system with a C
  toolchain.

For static binaries on Linux:

```
cajeta build --target=x86_64-unknown-linux-musl
```

This pulls musl-libc and statically links. The resulting binary runs
on any Linux kernel of sufficient version (≥3.2 for the runtime's
ucontext usage) with no glibc dependency.

For Windows + signed binaries:

```
cajeta build --target=x86_64-pc-windows-msvc \
             --sign-cert path/to/cert.pfx \
             --sign-password ...
```

Signing is a post-link step the cajeta build tool wraps around the
compiler. It is not a compiler feature itself.

---

## Optimization

> **Status.** The current binary exposes exactly one optimization
> knob, `--opt=O0|O1|O2|O3`. The default is `O0`, and the
> `--release`/`--fast` modes raise it to `O2`/`O3`. The
> `-Os`/`-Oz`/`-Ofast` levels, LTO, PGO, the cajeta-specific IR
> passes, and the IR-level efficiency knobs described below are
> the design target and are not yet wired. The `O2`/`O3` paths run
> LLVM's standard per-module pipeline (including LoopVectorize +
> SLP). See `src/cajeta/compile/Optimizer.{h,cpp}`.

### `-O` levels (planned beyond O0–O3)

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
the whole program. It gives big wins for stdlib + user-code call
sites that would otherwise stop at the archive boundary.

```
cajeta build -O2 --lto=thin       # ThinLTO — fastest LTO, scales well
cajeta build -O2 --lto=full       # Full LTO — best optimization, slow link
cajeta build -O2 --lto=off        # No LTO (default)
```

ThinLTO is the recommended choice for production builds. It gives
close to full-LTO performance with much faster link times. Full LTO is
worthwhile only for the very-final-shipping release of a perf-
sensitive program.

### Profile-guided optimization (PGO)  PGO is two-pass:

```
# Pass 1: instrumented build
cajeta build --pgo=instrument -o build/instrumented

# Run the instrumented binary against representative workloads;
# .profraw files get written to ./pgo-data/
./build/instrumented [args...]

# Pass 2: optimized build using collected profile
cajeta build --pgo=use=pgo-data/ -O2
```

PGO is worth ~5-15% on top of -O2 for typical workloads. It is
particularly effective for the JIT-style dispatch cajeta does
(vtable lookups, fiber scheduling), because the optimizer learns
hot inline candidates and lays out hot branches favorably.

### Cajeta-specific IR passes

These are run by the cajeta compiler before handing IR to LLVM, so
LLVM's own passes see already-cleaned IR:

- **Drop-chain elimination.** Local owners whose drop is never
  observable (the value doesn't escape, no exception path, drop
  fn is a no-op) skip the runtime drop_push / drop_pop_run pair.
- **Vtable devirtualization for sealed classes.** A call through
  a `sealed` class hierarchy with one implementer is rewritten as
  a direct call. LLVM does some of this on its own but the cajeta
  pass has the class-hierarchy info before LLVM sees the IR.
- **Structural-hash inlining.** Compiler-synthesized
  `Object.hash()` calls get the field-walk inlined at the call
  site when the receiver's runtime type is known. This cuts the
  call overhead for hash-flooded loops (HashMap probes, sorting, etc.).
- **Template-instantiation cache.** Repeated instantiations of the
  same template with the same type arguments share one LLVM module.
  This avoids combinatorial code bloat on programs with many
  `Box<T>` / `Optional<T>` style wrappers.

These are always on at `-O1` and above. Disable them individually
via `--pass-disable=<name>` for debugging compiler issues.

### IR-level efficiency knobs

- `--inline-threshold=<n>` overrides LLVM's default inline-cost
  threshold. A higher value means more inlining, which is
  typically faster and bigger. The default tracks the `-O` level.
- `--vectorize=on|off|loop|slp` controls LLVM auto-vectorization.
  The loop vectorizer and SLP (superword-level parallelism) are
  both on at -O2+.
- `--unroll=on|off` controls loop unrolling.
- `--debug-info=full|line|off` controls debug records, in Cajeta's
  own encoding (never DWARF). `line` (the default) keeps the
  shadow stack, so a trace resolves to `Type.method(File.cajeta)`.
  That is the frame's identity, at parity with an uninstrumented
  build. `full` adds the exact `:NN`, statement safepoints, local
  records, and the embedded location table an external debugger
  reads. `off` emits none of it, for the smallest binaries.

  The line number sits under `full` because it is the expensive
  part, and `line` leaves it out. The per-statement probe that
  carries it measured 3.5-9.4x at -O3, while the per-call probe
  that names the frame measured at parity. See `LineInfoCodegen::emitLineMark`.

### Tradeoffs to know

- **`-O3` vs `-O2` is not always faster.** -O3's extra inlining
  can blow out the instruction cache on hot loops. Benchmark
  before assuming -O3 wins.
- **LTO multiplies link time.** ThinLTO is ~2-3× a non-LTO link,
  and full LTO is ~10-20×. Iteration speed during development
  drops meaningfully. We recommend LTO only for tagged releases.
- **PGO is workload-specific.** A profile collected on one
  workload (e.g. CPU-bound batch processing) misses the right
  inline targets for a different workload (e.g. latency-bound
  request handling). Collect profiles representative of the
  shipping workload. Arbitrary microbenchmarks are the wrong
  input.

---

## Compiler flag index

Verified against `src/main.cpp` (the `cajeta` compiler binary's
argument parser) and `cajeta --help`. The build tool (`cajeta build`
/ `cajeta test`) is a separate wrapper described in
[`BuildTool.md`](BuildTool.md), and the flavor presets below are
compiler-side flags. Flags marked **(planned)** appear in earlier
drafts of this spec but are not parsed by the current binary.

### Invocation

The compiler takes three positional arguments after the options:

```
cajeta [options] <entry-method> <source-root> <archive-root>
```

`<entry-method>` is the dotted `package.Class.method` form (e.g.
`demo.App.run`). A `::`-separated form is not accepted.
`<source-root>` is the directory walked for `.cajeta` sources, and
`<archive-root>` is the stdlib/archive root. There are no
`--source-root` / `--entry-method` flags, and those positions are
mandatory.

### Source / output

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `-o <path>`                   | Output path for the final artifact (`.cja` for `--emit=cja`/`uber`, executable for `--emit=exe`). When unset, the compiler derives a default. |
| `--classpath=<paths>`         | Comma-separated list of `.cja` archive paths to ingest at compile-start. Each archive's `ClassSource` entries are re-parsed into the consumer's compile so user code can `import` their classes; the archive's bitcode is also bundled into `--emit=uber` output under `deps/<name>-<version>/`. Flag is repeatable; commas split within each occurrence. See [Classpath ingestion](#classpath-ingestion). |
| `--profile=<name>`            | Active `@Profile` for component gating (dev/test/release/…; default none). Selects which `@Profile`-annotated components compile in. **Not** a build-flavor selector — that is `--mode`/`--release`/etc. |

### Emit mode

| Flag                | Description                                                                  |
|---------------------|------------------------------------------------------------------------------|
| `--emit=ir`         | Exploded text LLVM IR (`.ll`) per module. **Default.**                       |
| `--emit=obj`        | Exploded native object files (`.o` / `.obj`) per module.                     |
| `--emit=cja`        | Cajeta archive (`.cja`) — project-only library form. No stdlib, no deps.     |
| `--emit=uber`       | Cajeta archive (`.cja`) — project + stdlib + transitively-referenced deps under `deps/<name>-<ver>/`. Self-contained / runnable. |
| `--emit=exe`        | Linked native executable.                                                    |
| `--prune-uber=on|off` | When `--emit=uber`, bundle only classpath entries transitively referenced by user/stdlib bitcode. Default `on`; `off` bundles everything. |

### Target

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--target=<triple>`           | LLVM target triple. Default: host triple.      |
| `--cpu=<name>`                | CPU model within target (e.g. `skylake`, or `native` for the host CPU + features ≈ `-march=native`). Default: `generic`. |
| `--features=<list>`           | Comma-separated CPU features (`+avx2,+bmi2`).  |

### Mode + optimization

Build-flavor selection is a single `--mode=<name>` (or its alias
flags). It sets both the safety/diagnostic toggle defaults and the
IR optimization level. See [`CompilerModes.md`](CompilerModes.md) for the
full per-feature breakdown.

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--mode=debug|debug-release|release|fast|minimal` | Build flavor. Also available as alias flags `--debug` / `--debug-release` / `--release` / `--fast` / `--minimal`. Default `debug`. |
| `--opt=O0|O1|O2|O3`           | IR optimization level for `--emit=obj`/`exe`. Default `O0`; `--release`/`--debug-release` imply `O2`, `--fast` implies `O3`. The only optimization-level knob — there is no `-O0`/`-Os`/`-Ofast`, and no `--lto` / `--pgo` / `--inline-threshold` / `--vectorize` / `--unroll` / `--pass-disable` **(all planned)**. |

### Code generation / safety (per-feature overrides)

These override the mode default after it expands. Full semantics in
[`CompilerModes.md`](CompilerModes.md).

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--bounds=on|off|trap`        | Array bounds-check generation. Default `on` (off in fast/minimal). |
| `--null-checks=on|off|trap`   | Null-receiver checks. Default `on` (off in minimal). |
| `--overflow-checks=on|off|wrapping` | Integer overflow behavior. Default `on` in debug, `wrapping` in release/fast/minimal. |
| `--source-tags=on|off`        | Carry alloc/drop source positions on chain entries. |
| `--poison-free=on|off`        | Sentinel-fill freed bytes.                     |
| `--live-set=strict|bounded|off` | Live-allocation set discipline.              |
| `--drop-chain-validate=on|off` | Per-push/pop integrity checks.                |
| `--ub-traps=on|off`           | Trap before would-be UB.                       |
| `--use-after-move-rt=on|off`  | Runtime backup for the static use-after-move check. |
| `--stack-trace-capture=on|off` | `backtrace(3)` at throw site.                 |
| `--profile-counters=on|off`   | Per-method PGO-collection instrumentation.     |

Debug records are controlled by `--debug-info=off|line|full` (see
Optimization above) and carry Cajeta's own encoding. The compiler
emits no DWARF at all. There is no `--frame-pointer` /
`--strip-symbols` flag **(planned)**.

### Diagnostics

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--diag-verbosity=terse|normal|verbose` | Compile-time diagnostic detail. Default `verbose` in debug, `normal` in release, `terse` in minimal. |
| `--diag-hints=on|off`         | "Did you mean…" suggestions.                   |

`--warn` / `--warn-error` / `--Werror` / `--diagnostic-format` /
`--time-passes` are **planned**. Warnings-as-errors is configured
in the build tool's manifest `settings.lint` block instead.

### Reproducible builds

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--source-date-epoch=<ts>`    | Fixed build timestamp (SOURCE_DATE_EPOCH).     |
| `--debug-prefix-map=<from>=<to>` | Remap source paths in embedded names / debug info. |
| `--seed=<hex>`                | Deterministic salt for any build RNG.          |

### XPU (GPU compute)

| Flag                          | Description                                    |
|-------------------------------|------------------------------------------------|
| `--xpu-backend=<list>`        | Device backend(s) for `@Kernel` methods: `none|nvptx|amdgpu|vulkan|cpu`, comma-separated. Default `none`. |
| `--xpu-arch=<arch>`           | Device arch (nvptx SM, amdgpu GFX, or vulkan SPIR-V env). |
| `--xpu-emit=none|ptx|cubin|isa|hsaco|spirv|spvasm|obj` | Also drop a per-kernel device artifact for inspection. Default `none`. |

See [`gpu/CajetaGPU.md`](../gpu/CajetaGPU.md).

### Archive operations

These are reached through `cajeta archive <subcommand>`, and there
is no `cja`/`car` binary. All twelve subcommands (`list`, `cat`,
`extract`, `info`, `deps`, `verify`, `diff`, `repack`, `strip`,
`merge`, `sign`, `verify-sig`) are implemented in
`src/cajeta/cli/ArchiveCommands.cpp`. The full reference is
[`ArchiveManagement.md`](ArchiveManagement.md).

---

## Open questions

- **Should the build tool be a separate binary or part of the
  compiler?** Rust splits `rustc` (compiler) from `cargo` (build
  tool). Java keeps `javac` separate from `maven` / `gradle`
  (third-party). Cajeta could go either way. The lean is a
  separate binary (`cajeta` build tool wrapping `cajetac`
  compiler), because it keeps the compiler focused on translating
  source to artifacts and lets the build tool evolve dependency /
  project management independently.
- **Format-version compatibility window.** How many `.cja` format
  versions back should the compiler accept? Rust's rlib format
  effectively requires a matched compiler version, and Java's
  `.class` format supports decades-back class files. The lean is
  to support N-2 (the two prior format versions) and document the
  deprecation window per release.
- **Distributed archive registry.** Cajeta users need some way to
  share archives without manual `lib/` vendoring. A simple HTTP
  registry (npm-style) is the obvious answer but commits to
  significant infrastructure. Defer it to a separate
  `cajeta-registry` RFC.
- **Reproducible builds.** Same source + same compiler + same
  flags should give byte-identical archive output. Build
  timestamps in the manifest break this, so offer
  `--source-date-epoch` (Reproducible Builds standard) to pin the
  timestamp from an env var.
- **Cross-archive symbol versioning.** When a stdlib archive's
  method signature changes, every user archive referencing it
  needs to be rebuilt or it crashes at link/load time. Solutions
  range from "ignore the problem, recompile everything" to "ABI
  version per archive entry, compatibility checks at load." The
  lean is toward the simple end for v1, and revisit it when
  real-world ecosystems make the problem painful.
