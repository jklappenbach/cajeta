# ArchiveManagement.md

Specification for `cajeta archive`, the CLI surface for inspecting,
extracting, transforming, and verifying `.cja` (Cajeta ARchive) files.

The `.cja` format itself is defined in [`Compilation.md` § Archive
format](Compilation.md#archive-format). This doc covers the **tooling
side**: what operations a user can perform on an existing `.cja`,
the CLI shape those operations take, and the conventions (exit
codes, JSON schemas, stdin / stdout piping) that the subcommands
honor.

The operations are reachable via `cajeta archive <subcommand>` today.
When the umbrella build tool spec'd in [`BuildTool.md`](BuildTool.md)
ships and the binary splits (`cajetac` for the compiler back end,
`cajeta` for the build tool), the archive subcommands migrate to the
build-tool side — the implementation lives in `libcajeta_archive`,
so only the CLI dispatch moves.

## Table of contents

1. [Design goals](#1-design-goals)
2. [Subcommand catalog](#2-subcommand-catalog)
3. [Full CLI reference](#3-full-cli-reference)
4. [Exit codes](#4-exit-codes)
5. [Output formats](#5-output-formats)
6. [Pipe conventions](#6-pipe-conventions)
7. [Examples](#7-examples)
8. [Signing](#8-signing)
9. [Open questions](#9-open-questions)

---

## 1. Design goals

- **One tool, subcommand-shaped.** `cajeta archive list ...`,
  `cajeta archive cat ...`, ... All operations under the same noun
  so help is discoverable from one place.
- **Pipeable by default.** Every subcommand that produces inspectable
  output supports `--json` for structured machine consumption. Every
  subcommand that consumes or produces archive bytes accepts `-` for
  stdin/stdout where it makes sense, so `curl ... | cajeta archive
  cat - manifest.json | jq .kind` works end-to-end.
- **Exit codes are load-bearing.** `verify` and `verify-sig` set exit
  codes a CI pipeline can switch on. Successful operations return 0;
  every failure category gets a distinct nonzero code (§4).
- **No reformatting of archive bytes by default.** Operations that
  transform an archive (`repack`, `strip`, `merge`) are explicit
  about it; operations that read (`list`, `cat`, `info`, `extract`)
  never mutate the source file.
- **Concurrency-safe reads.** Multiple `cajeta archive` invocations
  on the same `.cja` file from different processes are safe — reads
  open `O_RDONLY`, no lock, no temp files in the source archive's
  directory.

---

## 2. Subcommand catalog

| Subcommand                  | Purpose                                                          |
|-----------------------------|------------------------------------------------------------------|
| [`list`](#31-list)          | Show entries + per-entry metadata (name, kind, origin, sizes)    |
| [`cat`](#32-cat)            | Dump one entry's bytes to stdout                                 |
| [`extract`](#33-extract)    | Explode entries to a directory                                   |
| [`info`](#34-info)          | Print the manifest (kind, name, version, build info, deps count) |
| [`deps`](#35-deps)          | Print the `deps` array (uber archives)                           |
| [`verify`](#36-verify)      | Structural integrity + entry-checksum recomputation              |
| [`diff`](#37-diff)          | Entry-by-entry diff between two archives                         |
| [`repack`](#38-repack)      | Re-emit with different compression level / algorithm             |
| [`strip`](#39-strip)        | Drop entries by name glob                                        |
| [`merge`](#310-merge)       | Combine archives into one (ad-hoc uber bundling)                 |
| [`sign`](#311-sign)         | Detached ed25519 signature over the archive bytes                |
| [`verify-sig`](#312-verify-sig) | Verify a detached signature against a public key             |

All twelve subcommands ship in the current implementation. Signing
uses OpenSSL libcrypto for PEM key parsing + ed25519 sign/verify;
keys are interchangeable with OpenSSL's `openssl genpkey -algorithm
ed25519` output.

---

## 3. Full CLI reference

Global flags (apply to every subcommand):

```
--json                  JSON output (default: human-readable text).
--quiet, -q             Suppress non-error output. Exit code is the
                        sole signal.
--color=on|off|auto     Color in human-readable output. Default auto
                        (on if stdout is a tty).
```

### 3.1 `list`

```
cajeta archive list <archive> [paths...] [flags]
```

Print one row per entry, sorted by name. With `[paths...]` arguments,
filter to entries whose name exactly matches or sits under the given
path prefix. Globs (`*`, `**`, `?`) supported.

**Output (human):**

```
KIND      ORIGIN  SIZE     COMPRESSED  NAME
class_bc  user    12345    4321        tour/Tour.bc
class_bc  user    8901     2510        tour/AllocationDemo.bc
class_bc  stdlib  64512    18234       cajeta/lang/String.bc
class_src user    1024     420         tour/Tour.cajeta
resource  user    256      178         resources/banner.txt
```

**Output (`--json`):**

```json
{
  "archive": "Tour.cja",
  "entry_count": 27,
  "entries": [
    {
      "name": "tour/Tour.bc",
      "kind": "class_bitcode",
      "origin": "user",
      "size": 12345,
      "compressed_size": 4321
    },
    ...
  ]
}
```

**Flags:**

```
--sort=name|size|kind|origin   Default name.
--reverse, -r                  Reverse sort.
--long, -l                     Adds checksum (xxh3) column to text output.
```

### 3.2 `cat`

```
cajeta archive cat <archive> <entry-path>
```

Decompress one entry's bytes and write them to stdout. The bytes are
**raw** — for `class_bitcode` entries that's LLVM bitcode (binary);
for `class_source` entries that's UTF-8 cajeta source; for `resource`
entries it's whatever the original file was. Pipe-friendly: combine
with `llvm-dis -`, `jq`, `xxd`, etc.

`<archive>` accepts `-` to read the archive from stdin (the whole
archive is consumed into memory; not appropriate for huge archives
streamed from sockets).

Exits 0 on success, [`E_NOT_FOUND` (§4)](#4-exit-codes) if the named
entry isn't in the archive.

### 3.3 `extract`

```
cajeta archive extract <archive> [-C <dir>] [paths...] [flags]
```

Decompress entries to disk under `<dir>` (default: `.`). With
`[paths...]`, only extract entries matching one of the path
arguments — same path / glob rules as `list`. Files are written with
their entry name as the relative path (`/` becomes the OS separator;
parent directories are created on demand).

**Flags:**

```
-C <dir>            Destination directory. Default: current working dir.
--overwrite         Overwrite existing files. Default: skip + warn.
--strip=<n>         Drop n leading path components from each entry name.
--flatten           Strip all path components — write every entry to <dir>
                    using just its basename. Mutually exclusive with --strip.
```

Output (when not `--quiet`): one line per extracted file.

### 3.4 `info`

```
cajeta archive info <archive> [flags]
```

Print the manifest's metadata block — no entry listing, no payload.

**Output (human):**

```
name:                Tour
version:             0.1.0
kind:                uber
cajeta_lang_version: 1.0
build_flavor:        release
build_timestamp:     2026-05-26T16:08:14Z
target_triple:       x86_64-unknown-linux-gnu
entry_count:         27
deps_count:          1
total_size:          524288 bytes (compressed: 425127 bytes, ratio 1.23×)
```

**Output (`--json`):** the manifest's JSON, verbatim — guaranteed
schema-compatible with the manifest written by the compiler. Adding
fields to the manifest format is backward-compatible; consumers
ignore unknown keys.

> **Note (v1).** The compiler currently writes a *minimal* manifest —
> `name`, `version`, `kind`, `format_version`, `entry_count`, and (for
> uber) `deps`. `info` scans for the richer fields shown above
> (`build_flavor`, `build_timestamp`, `target_triple`,
> `cajeta_lang_version`, `total_size`) and prints them blank/derived
> when the producing archive didn't carry them. They become populated
> as the writer grows to emit them; the field set is additive.

### 3.5 `deps`

```
cajeta archive deps <archive> [flags]
```

Just the `deps` array of the manifest. Empty array on `cja` (project-
only) archives; nonempty on `uber` archives.

**Output (human):**

```
cajeta-stdlib  1.0.0  142 entries
cajeta-math    0.4.2  88 entries
```

**Output (`--json`):**

```json
[
  { "name": "cajeta-stdlib", "version": "1.0.0", "included_entry_count": 142 },
  { "name": "cajeta-math",   "version": "0.4.2", "included_entry_count": 88 }
]
```

### 3.6 `verify`

```
cajeta archive verify <archive>
```

Validates the archive without extracting it. Checks:

1. **Header magic** is `CAJETA01`.
2. **Format version** is supported (≤ writer's max).
3. **Manifest** decompresses and parses as valid JSON with the
   required fields (`name`, `version`, `kind`).
4. **Each entry** decompresses, its on-disk `compressed_size` matches
   what the header claimed, its `uncompressed_size` matches the
   decompressed length.
5. **Per-entry xxh3 checksum** (when present in the trailing index)
   matches the recomputed hash of the decompressed bytes.
6. **No duplicate entry names.**
7. **Index entries** (when present) point at valid offsets.

Prints one line per check (or stays silent under `--quiet`). Exits 0
on success; exits with the most-relevant nonzero code from §4 on
first failure.

**Flags:**

```
--strict          Fail if the archive's `format_version` exceeds the
                  reader's known-handled max, OR if any optional but
                  recommended field is absent (build_timestamp, etc.).
```

### 3.7 `diff`

```
cajeta archive diff <a.cja> <b.cja> [flags]
```

Entry-by-entry comparison. By default uses xxh3 of the decompressed
bytes — two entries with the same name but different content show as
`changed`.

**Output (human):**

```
+ tour/AsyncDemo.bc           (added in b)
- tour/OldDemo.bc             (removed from b)
~ tour/Tour.bc                (changed: 12345 → 12891 bytes, xxh3 differs)
  tour/AllocationDemo.bc      (identical)
  ...
```

**Output (`--json`):**

```json
{
  "added":   ["tour/AsyncDemo.bc"],
  "removed": ["tour/OldDemo.bc"],
  "changed": [{"name": "tour/Tour.bc", "a_size": 12345, "b_size": 12891, "a_xxh3": "...", "b_xxh3": "..."}],
  "identical_count": 25
}
```

**Flags:**

```
--name-only       Compare entry names only; ignore content.
--include-stdlib  Include stdlib-origin entries (default: exclude — diffs
                  on uber archives usually want only the user-visible delta).
```

Exits 0 if the archives are byte-equal in entry content; 1 if any
difference is detected (CI-friendly).

### 3.8 `repack`

```
cajeta archive repack <in.cja> <out.cja> [flags]
```

Re-emit the archive with a different compression configuration. The
manifest is preserved; only the on-disk layout changes.

**Flags:**

```
--zstd=<level>      zstd compression level (1-22). Default: 19.
--compression=none  Disable compression entirely (debugging / testing).
--keep-index        Rebuild the trailing index. Default: yes.
```

### 3.9 `strip`

```
cajeta archive strip <in.cja> <out.cja> [flags]
```

Drop entries by name glob, write the result. Useful for stripping
source entries before publishing (`--exclude="**/*.cajeta"` ships
bitcode-only), or for trimming an uber archive to the user-visible
slice (`--exclude="deps/**"`).

**Flags:**

```
--exclude=<glob>    Drop entries matching glob. Repeatable.
--include=<glob>    Keep only entries matching glob. Repeatable.
```

Exits with error if `--exclude` would drop entries the manifest's
metadata still references.

### 3.10 `merge`

```
cajeta archive merge <out.cja> <a.cja> <b.cja> [<c.cja>...] [flags]
```

Combine multiple archives into one. Useful for ad-hoc uber-bundling
outside the compiler — e.g. a fixture builder that wants to bundle
test data with compiled code.

**Flags:**

```
--name=<name>       Manifest name for the output (required if archives disagree).
--version=<ver>     Manifest version for the output.
--kind=uber|cja     Manifest kind for the output. Default: uber.
--prefix-deps       Nest each input archive under deps/<name>-<version>/
                    (mirrors the compiler's --emit=uber layout).
--allow-collisions  When two inputs have an entry with the same name and
                    different content, take the one from the rightmost
                    archive. Default: error on collision.
```

---

## 4. Exit codes

| Code | Constant            | Meaning                                                                      |
|------|---------------------|------------------------------------------------------------------------------|
| 0    | `OK`                | Success.                                                                     |
| 1    | `E_USAGE`           | CLI usage error: unknown subcommand, missing required argument.              |
| 2    | `E_NOT_FOUND`       | Named entry / file does not exist.                                           |
| 3    | `E_BAD_MAGIC`       | File doesn't start with the `CAJETA01` magic.                                |
| 4    | `E_UNSUPPORTED_FMT` | Format version exceeds the reader's max.                                     |
| 5    | `E_TRUNCATED`       | File ends mid-record (truncated on-disk).                                    |
| 6    | `E_CORRUPT`         | Decompression or parsing failure — file is structurally invalid.             |
| 7    | `E_CHECKSUM`        | Per-entry xxh3 mismatch (only when the trailing index supplies checksums).   |
| 8    | `E_IO`              | OS I/O failure (read, write, permission denied).                             |
| 9    | `E_DIFF`            | `diff`-only: archives differ. Returned even on a clean diff run.             |
| 10   | `E_COLLISION`       | `merge`-only: an entry name appeared in two inputs without `--allow-collisions`. |
| 11   | `E_SIG_INVALID`     | `verify-sig`-only: signature failed verification.                            |

Other subcommands that detect failures (`--strict verify`, `extract`
with an existing target file and no `--overwrite`) reuse the most
applicable code above.

---

## 5. Output formats

Two output formats are supported uniformly across read subcommands:

- **Human** (default). Aligned columns, optional color, intended for
  interactive use. Width adapts to terminal width when stdout is a
  tty; uses a fixed reasonable width when piped.
- **JSON** (`--json`). One JSON document on stdout. The schema for
  each subcommand is documented in §3. Stable across point releases;
  additions are backward-compatible.

`--json` is the only format guaranteed stable across versions. The
human-readable format may evolve (column reorderings, additional
hint lines); scripts that depend on output should use `--json`.

---

## 6. Pipe conventions

- `<archive>` accepts `-` to read from stdin where it makes sense
  (`cat`, `list`, `info`, `deps`, `verify`). The archive is buffered
  into memory; reading multi-gigabyte archives from stdin is
  supported but slow.
- `<out.cja>` accepts `-` to write to stdout. The output is binary;
  combine with redirection (`> file.cja`) or pipe into another tool.
- `cat`'s stdout is always raw binary — no formatting, no
  termination character, no encoding conversion.
- All other read subcommands write text (or JSON with `--json`) to
  stdout. Errors write to stderr.
- `--quiet` suppresses informational lines on stdout; errors still
  land on stderr.

---

## 7. Examples

**Inspect what an archive contains:**

```sh
cajeta archive list Tour.cja
cajeta archive info Tour.cja
```

**Pull the manifest as JSON for scripting:**

```sh
cajeta archive info Tour.cja --json | jq '.kind, .deps_count'
```

**Print a specific bitcode entry and disassemble it on the fly:**

```sh
cajeta archive cat Tour.cja tour/Tour.bc | llvm-dis - | head
```

**Find the largest entries in an archive:**

```sh
cajeta archive list Tour.cja --json \
    | jq '.entries | sort_by(-.size) | .[0:5] | .[] | "\(.size)\t\(.name)"' -r
```

**Extract source files from a `cja` archive to a directory:**

```sh
cajeta archive extract Tour.cja -C /tmp/tour-src "**/*.cajeta"
```

**CI guard: fail if the published archive's user-visible entries
have changed without a version bump:**

```sh
cajeta archive diff baseline.cja Tour.cja --name-only
# exit code 9 if differences exist
```

**Strip the stdlib bundle from an uber archive (rebuild as a thin
cja that depends on the system stdlib):**

```sh
cajeta archive strip Tour.cja Tour-thin.cja --exclude="cajeta/**"
```

**Round-trip an archive's manifest into a new copy with stronger
compression:**

```sh
cajeta archive repack Tour.cja Tour-max.cja --zstd=22
```

**Bundle test fixtures into an archive next to compiled code:**

```sh
cajeta archive merge bundle.cja \
    Tour.cja \
    fixtures.cja \
    --name=tour-tests \
    --version=0.1.0
```

---

## 8. Signing

`sign` and `verify-sig` produce and validate **detached ed25519
signatures** over the archive bytes. Implementation uses OpenSSL
libcrypto (`EVP_PKEY` / `PEM_read_bio_*`); keys are PEM-encoded
and round-trip through `openssl genpkey -algorithm ed25519`.

### 3.11 `sign`

```
cajeta archive sign <archive> --key <ed25519.pem> [--out <archive.sig>]
```

Generates a detached 64-byte ed25519 signature over the archive's
raw bytes and writes it to `<archive>.sig` (or the path given via
`--out`). The signature file contains the raw 64 bytes — no
header, no encoding, no length prefix.

The private key must be ed25519-flavored (PKCS#8-wrapped is the
common shape, what `openssl genpkey -algorithm ed25519` emits).
Non-ed25519 keys (RSA, ECDSA, etc.) are rejected with a clear
error pointing at the expected algorithm.

`<archive>` accepts `-` for stdin; when stdin is used, `--out` is
mandatory (we can't infer a `.sig` filename without a source path).

### 3.12 `verify-sig`

```
cajeta archive verify-sig <archive> --pubkey <pem> [--sig <archive.sig>]
```

Verifies the detached signature against a public key. Exits 0 when
the signature validates, exits 11 (`E_SIG_INVALID`) on any mismatch
or on OpenSSL-side parse / verify failures. Public key is PEM-encoded
(`openssl pkey -in <priv> -pubout` emits the matching shape).

`--sig` defaults to `<archive>.sig`. `<archive>` accepts `-` for
stdin; when stdin is used, `--sig` is mandatory.

### Trust roots and key distribution

The current implementation is intentionally key-agnostic: the user
brings their own public key and decides whether to trust it. That
matches the minimal "checksum-on-steroids" use case — a publisher
generates a keypair, distributes the pubkey via whatever channel
their consumers already trust, and signs every release.

For richer trust models — PGP-style web of trust, per-registry
pinned public keys, sigstore / cosign-style transparency logs — the
hook is `BuildTool.md`'s [repository
protocol](BuildTool.md#7-repositories): a registry can advertise a
pinned public key for each archive, and the build tool calls
`cajeta archive verify-sig` automatically during ingestion. That
integration lands when the build tool ships.

### Signature storage

Detached `.sig` files are the only supported form today. A header
flag bit is reserved in the `.cja` container for "signature embedded
after the trailing index" — adding that as a future option only
requires writer/reader support, not a format-version bump.

---

## 9. Open questions

- **Index-checksum schema.** The v1 trailing index is a compact
  binary record per entry (name, offset, on-disk size) with **no**
  checksum slot, so `verify` and `diff` recompute xxh3 from the
  decompressed bytes on load. A richer v2 index could carry a per-
  entry `checksum_xxh3`, unlocking "fast verify without decompression
  for files where we trust the producer." Decide whether `repack`
  would populate it by default.
- **`cajeta archive cat` for stdlib entries inside uber archives.**
  An uber archive has both `tour/Tour.bc` (user) and
  `cajeta/lang/String.bc` (stdlib). Should `cat cajeta/lang/String.bc`
  Just Work, or require a `--include-stdlib` flag, mirroring `diff`'s
  default exclusion? Current sketch: Just Work (no flag) — `cat` is
  for inspection, hiding entries surprises users.
- **Symlinks during `extract`.** The `.cja` format has no symlink
  entry kind today. If we add one later, `extract` needs a
  `--no-symlinks` for safety (zip-slip / tar-slip equivalents). Flag
  reserved for a future format extension even if no symlink entries
  exist yet.
- **`cajeta archive merge` and origin tags.** When merging two
  user-origin archives, the output is also user-origin. When merging
  a user archive with a stdlib archive (via `--prefix-deps`), the
  stdlib entries pick up `Origin::Dependency`. Worth a per-input
  origin override flag.
- **Where does the help text live?** Currently `cajeta --help` shows
  compile flags; subcommand help would be `cajeta archive --help`
  and `cajeta archive <subcommand> --help`. The dispatching pattern
  matches `git`/`cargo`/`docker`; the help generator infrastructure
  to do it properly is BuildTool.md-adjacent. v1 implementation can
  hard-code subcommand help.
