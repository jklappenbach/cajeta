---
id: cajeta-driver-skill-discovery
applies-to: [cajeta/toolchain/skill-discovery, cajeta-driver/search-skill, cajeta-driver/list-skills, cajeta-driver/get-skills]
title: Skill discovery — search, list, get
description: Find and fetch the implementation skills shipped inside resolved cajeta dependencies, offline.
---
# Skill discovery (search / list / get)

These three `cajeta` subcommands read the **skills shipped inside resolved
dependencies** and answer "what guidance ships for this name, and give me the
text." All three are offline: they read `./cajeta.lock` + the local artifact
cache, never the network.

## Route by task

| You want to… | Run |
|---|---|
| Name → matching skill URIs (typo-tolerant) | `cajeta search-skill <name> [--version <v>] [--from <module>] [--exact]` |
| Enumerate skills (optionally one subtree) | `cajeta list-skills [<scope>] [--version <v>] [--from <module>]` |
| URI(s) → payload text | `cajeta get-skills <uri>[,<uri>...]` |

Normal agent flow: **`search-skill` to discover URIs → `get-skills` to fetch**.
A URI is a stable cache key (it pins the *resolved* version), so once you hold a
payload, skip the re-fetch. Use `list-skills` to browse when you don't have a
name to search.

## Prerequisites (shared, both cause exit 1)

- `./cajeta.lock` must exist in the **current working directory** (paths are
  relative to cwd, not the manifest). A missing/unreadable lockfile prints
  `cajeta <cmd>: <error>` and exits 1.
- Artifacts must be in the local `ArtifactCache` (cache root `.`). A package
  whose `.cja` is not cached is silently skipped for search/list (it just
  contributes no results); for `get-skills` an uncached coordinate is a per-URI
  error.

## search-skill — name → URIs

Fuzzy (Damerau–Levenshtein) over **both** canonical names and titles; name
matches are segment-aware and outrank title matches at equal distance.
Hierarchical/prefix-inclusive: a query surfaces the name's own skills, its
descendants' (a package query reaches its classes/methods), and the
nearest-ancestor overview. Match tiers, best first: `exact` → `descendant` →
`ancestorOverview`. `--exact` disables fuzzy matching.

```console
$ cajeta search-skill cajeta/io/Fiel.open      # typo in 'File' tolerated
cja-skill://cajeta.io@1.4.2/file-open	cajeta/io/File.open
```

Default text output: one line per result, `<uri>\t<matchedName>`.
Exactly one positional `<name>` is **required**.

Version selection: `--version` restricts to that resolved version (overrides
`--from`); `--from <workspace-member>` infers the version that member sees; with
neither, a diamond (two resolved versions) returns **both, version-tagged** — no
silent pick.

## list-skills — enumerate

Optional `<scope>` is an **exact** library/package prefix (NOT fuzzy), inclusive
of the subtree; no scope lists every skill in the resolved set. Same
`--version`/`--from` semantics as search. Text output: `<uri>\t<title>` per line,
ordered by URI.

```console
$ cajeta list-skills cajeta/io
cja-skill://cajeta.io@1.4.2/file-open	Opening files
```

## get-skills — URI(s) → payload

One **comma-joined** positional argument: `uri1,uri2,...` (whitespace trimmed,
empties dropped). Each URI is `cja-skill://<library>@<version>/<skill-id>` where
`<version>` is the resolved version (so Get is always exact). Resolves each
coordinate through `cajeta.lock` to a local `.cja` and prints the verbatim
payload (frontmatter + body), labeled `# <uri>`. A bad/uncached/missing URI is a
per-URI error to stderr — the other URIs still print.

```console
$ cajeta get-skills cja-skill://cajeta.io@1.4.2/file-open,cja-skill://cajeta.net@2.0.0/connect
# cja-skill://cajeta.io@1.4.2/file-open
---
id: file-open
...
# cja-skill://cajeta.net@2.0.0/connect
...
```

## --json (all three)

Pass `--json` for machine output (the `tools/mcp` wrapper consumes this):
- search: array of `{uri, matchedName, tier, distance}`.
- list: array of `{uri, title, names: [...]}`.
- get: array of `{uri, ok, payload?|error?}`.

## Exit codes

- `0` success (for search: at least one match).
- `1` operational: **no match** (search), lockfile/context load error, or any
  per-URI failure in get (`--json` get still prints results, exits 1).
- `2` bad args: unknown flag, missing/extra positional, missing `<name>`, empty
  URI list, or `get-skills` with no URI argument (prints the usage string).

## What these do NOT do

- They do **not** resolve/install dependencies or write the lockfile — run the
  build/resolve commands first so `cajeta.lock` and the cache are populated.
- They do **not** fetch over the network or author/edit skills (skills are
  hand-written `skills/*.md` packaged into the `.cja` at build).
- `list-skills` scope and `get-skills` URIs are **exact**, not fuzzy — only
  `search-skill` is typo-tolerant.
- No per-skill version exists; a skill is versioned with its owning library.
