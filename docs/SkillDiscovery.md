# Skill discovery

Skills are short, hand-authored guides that travel **inside** a library's `.cja`
archive and tell an agent (or a human) how to implement against that library's
symbols. Skill discovery lets you ask, by canonical name, "what guidance ships
for this?" and fetch it — entirely offline, from the resolved dependency set
(plus the always-available stdlib skills baked into the compiler — see below).

See `docs/specs/skill-discovery-spec.md` for the full specification.

## The three operations

| CLI | Library API (`cajeta::buildtool::skill`) | Purpose |
|-----|------------------------------------------|---------|
| `cajeta search-skill <name> [--version <v>] [--from <module>] [--exact]` | `searchSkills(...)` | name → matching skill URIs |
| `cajeta list-skills [<scope>] [--version <v>] [--from <module>]` | `listSkills(...)` | enumerate available skills |
| `cajeta get-skills <uri>[,<uri>...]` | `getSkills(...)` | URI(s) → payload(s) |

The CLI is a thin transport over a transport-agnostic core, so the same calls
back a future MCP service (spec §6) without a rewrite.

**The agent workflow:** `search-skill` to discover URIs → `get-skills` to fetch
payloads. A URI is a stable cache key, so once you hold a payload, skip the fetch.

## Authoring a skill

Skills are **front-matter Markdown** authored by hand in a package's `skills/`
directory (one `*.md` per skill — clean git diffs):

```markdown
---
id: file-open
applies-to: [cajeta/io/File, cajeta/io/File.open]
title: Opening files
description: How to open and dispose File handles correctly.
---
# Opening files

Use `File.open` and dispose deterministically …
```

- `id` (required) — unique within the library.
- `applies-to` (required) — canonical-name bindings this skill aids. Four name
  kinds (spec §3.1): **library** (`cajeta.io`), **package** (`cajeta/torch/nn`),
  **class** (`cajeta/torch/nn/Linear`), **method** (`cajeta/torch/nn/Linear.forward`).
- `title` / `description` (optional) — summaries; `title` is also fuzzy-searchable.
- There is **no per-skill `version`** — a skill is versioned with its library.

## Packaging

At package build, `skills/*.md` are validated, indexed (`skills/index.json`), and
embedded as `skills/<id>.md` members in the `.cja`. An invalid skill fails the
build with a diagnostic naming the file and field. A package with no `skills/`
dir simply ships no index — nothing else changes.

### Compression (spec §2.4)

Skill members are stored **compressed** as part of the `.cja` protocol — the
archive zstd-compresses every entry on write and transparently decompresses on
read (each entry is framed `uint64 uncompressed_length || zstd_bytes`; the format
header flags `FLAG_ENTRIES_COMPRESSED`). Discovery reads decompressed payloads;
nothing on the read path sees compressed bytes. This is the compiler/build-tool's
own C++ `libzstd` (`zstd.h`), **not** the `cajeta.wire` decompressor (which is for
cajeta *programs*, not compiler internals).

## Stdlib skills are always available (spec §2.5)

The standard library's skills are **embedded in the compiler** (a compressed
corpus baked into the `cajeta` binary), so they are discoverable in **every**
project with **no `cajeta.lock` and no dependencies** — a missing lockfile is not
an error. `search-skill` / `list-skills` / `get-skills` always seed the embedded
stdlib archives in addition to whatever the lockfile resolves; `get-skills`
resolves an embedded stdlib URI directly from the baked-in corpus before any
`.cja` lookup.

Stdlib URIs carry the stdlib skill version (currently `1.0`):

```console
$ cajeta search-skill cajeta.process        # works in an empty dir, no lockfile
cja-skill://cajeta.process@1.0/process-overview	cajeta.process
```

The same holds through the MCP server (`searchSkills` / `getSkills`), which shells
out to these CLI subcommands.

## The URI scheme

```
cja-skill://<library>@<version>/<skill-id>
e.g.  cja-skill://cajeta.io@1.4.2/file-open
```

The `<version>` is the **resolved** library version (from the lockfile), so Get
is always exact and a held URI is a stable cache key across machines.

## Fuzzy matching

`search-skill` is typo-tolerant (Damerau–Levenshtein, so adjacent transpositions
cost 1) over **both** canonical names and titles:

```console
$ cajeta search-skill cajeta/io/Fiel.open      # typo in 'File'
cja-skill://cajeta.io@1.4.2/file-open	cajeta/io/File.open

$ cajeta search-skill "Openin files"            # typo in a title
cja-skill://cajeta.io@1.4.2/file-open	cajeta/io/File
```

Each result reports the **matched canonical name**. Name matches are
segment-aware (a typo in one segment doesn't force the others to fuzzy-match) and
outrank title matches at equal distance. `--exact` disables fuzzy matching.

Results are hierarchical and prefix-inclusive (spec §3.2): a query surfaces the
name's own skills, its descendants' skills (a package query reaches its classes
and methods), and the nearest-ancestor overview.

## Versions: diamonds and consumer scoping

When a build resolves **two versions of the same library** side by side, a bare
name maps to a set:

```console
$ cajeta search-skill foo/io/File              # foo@1.0 and foo@2.0 both resolved
cja-skill://foo@1.0.0/fo	foo/io/File
cja-skill://foo@2.0.0/fo	foo/io/File          # both, version-tagged — no silent pick

$ cajeta search-skill foo/io/File --version 2.0.0
cja-skill://foo@2.0.0/fo	foo/io/File          # restricted to one version
```

`--from <module>` infers the version from the **asking module's** resolution, so
two consumers in a diamond correctly see different versions (the module
identifier is a workspace member name). An explicit `--version` overrides
`--from`; with neither, the diamond default above applies.

## Get

```console
$ cajeta get-skills cja-skill://cajeta.io@1.4.2/file-open,cja-skill://cajeta.net@2.0.0/connect
# cja-skill://cajeta.io@1.4.2/file-open
---
id: file-open
...
# cja-skill://cajeta.net@2.0.0/connect
...
```

`get-skills` takes a comma-delimited URI list, resolves each `<library>@<version>`
(an embedded stdlib URI from the baked-in corpus, otherwise through the lockfile
to a local `.cja`), and prints each payload labeled by URI. A
bad or uncached URI is reported as a per-URI error without failing the others.
Everything is offline — Get never touches the network.
