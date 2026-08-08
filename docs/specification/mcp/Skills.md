# Skills — agent guidance that ships with the code

A **skill** is a hand-written implementation guide, keyed to a canonical name
(library, package, class, or method), that an AI agent reads *before or while*
it writes code against that symbol. Skills are part of the Cajeta distribution
format: **more than 180 ship embedded in the compiler**, and **every library
published for Cajeta releases with its skills inside its `.cja` archive**. The
[built-in MCP server](CompilerMcp.md) and the `cajeta search-skill` /
`list-skills` / `get-skills` subcommands are the two doorways into the same
corpus.

Specs: `specs/archive/skill-discovery-spec.md` (storage, URIs, matching),
`specs/archive/skill-authoring-spec.md` (per-level content rules).
Source: `src/cajeta/buildtool/skill/`.

## Why skills exist

An agent asked to write Cajeta arrives fluent in Java, C++, and Rust — and
those priors are exactly what hurt it. Cajeta looks like Java and behaves like a
value-semantics, no-GC core, so the failures are quiet: a borrow stored in a
field where a transfer was required, an element returned from a container that
still owns it, `try`-with-resources reached for in a language whose resource
pattern is drop-on-scope. That code compiles and then dies.

Documentation on a website does not solve this, because the agent is not reading
a website at the moment it writes the line. So the guidance travels with the
artifact and is retrievable over a protocol the agent already speaks.

### The minimality criterion

A skill earns a place only if its absence would make an otherwise-competent
agent:

- **(a) crash or corrupt** — code that compiles but dies (ownership, lifecycle,
  capture, transfer);
- **(b) fail to compile blind** — rejected for a reason the error text alone does
  not teach;
- **(c) silently miscompile intent** — code that runs and does the wrong thing;
- **(d) dead-end** — hunt for a capability that does not exist, or that lives
  somewhere else.

Anything the agent would infer correctly from Java intuition is deliberately
excluded — at most a routing row or a hazard line in an overview. The corpus is
a floor, not a reference manual; the [guide](../../guide/README.md) and
cajetadoc remain the full story.

## The format

One Markdown file per skill: YAML frontmatter (machine-facing, drives search and
indexing) over a Markdown body (the guidance).

```markdown
---
id: language-ownership
applies-to: [cajeta/language/ownership, cajeta/language/borrowing, cajeta/language/slices]
title: Ownership, borrowing, # transfer, drops, and slices
description: The rules that keep cajeta memory-safe at compile time — borrow by default, transfer with #, drop at scope exit — and the borrow-checker errors you will meet.
---

# Ownership & transfer — read this before storing, returning, or passing heap values
...
```

| Field | Meaning |
|---|---|
| `id` | Unique within the library; also the archive member stem (`skills/<id>.md`). Convention: level-descriptive — `io-file-overview`, `io-file-File`, `io-file-File-writeAllBytes`. |
| `applies-to` | The canonical name(s) this skill is bound to. This is what Search resolves. |
| `title` | Short summary; shown by `listSkills`, and fuzzy-matched alongside names. |
| `description` | One line; used in ranking and listings. |

There is **no per-skill version field**. A skill is versioned with the library it
describes: the URI's version is the library's *resolved* version, so the same URI
always yields the same bytes.

## Levels

Levels mirror the canonical-name kinds, with **component** inserted between
package and class as a named group of cooperating types. Higher levels *route*;
lower levels *detail*. One fact lives at exactly one level and is linked, not
copied — a duplicated fact rots.

| Level | Binds to | Carries | Selection |
|---|---|---|---|
| **Library** | `cajeta.io` | Purpose, a task → entry-point routing table (**with negative rows**), library-wide invariants (ownership, disposal, error style, threading), one canonical end-to-end example, hazards, disambiguation between overlapping options, setup/preconditions, pointers down | Exactly one per library |
| **Package** | `cajeta/io/file` | The slice this package owns, an inventory map separating entry-point from support types, intra-package collaboration, package-scoped recipes and invariants | Per package with real content |
| **Component** | a named group | How a set of classes collaborate across a workflow | When the interaction is the hard part |
| **Class** | `cajeta/io/file/File` | How to use this type: construction, ownership of what it returns, lifecycle, idiomatic example | Per non-obvious type |
| **Method** | `cajeta/io/File.open` | The sharp, the complex, the protocol-bearing — methods with ordering rules, ownership subtleties, or failure modes | Sparingly |

Every skill, at every level, must carry: the four frontmatter fields; a **worked
example** in real, idiomatic Cajeta with its `import` lines (ideally mirroring a
passing test); **ownership and lifecycle stated wherever a value crosses a
boundary** — `#` transfer at call sites, owned-vs-borrowed returns, who frees;
**links rather than copies** to related skills; and concision — lead with the
answer, no API dumps.

The authoring principle behind all of it: front-load routing and decisions, make
ownership explicit and early, state what the code does **not** do, and show one
real example rather than enumerating signatures.

## Storage

### In a library archive

Skills are authored by hand in `skills/*.md` at the package root. At build time
the toolchain discovers, validates, and indexes them, then embeds them into the
`.cja` alongside the compiled bitcode:

```
mylib.cja
├── … compiled bitcode members
├── skills/index.json       ← canonical-name → [id]; id → {title, member-path}
├── skills/opening-files.md
└── skills/retry-policy.md
```

A package with no `skills/` directory is not an error — it simply ships no
guidance. An invalid skill or a duplicate `id` **is** an error and fails the
build. Members are emitted sorted by path, so packaging is reproducible.

Skill members ride the `CAJETA01` archive's normal zstd path (framed
`uint64 uncompressed_length || zstd_bytes`), so they are compressed at rest and
decompressed only when read.

Because skills live inside one library's archive, a library carries skills only
for the canonical names **it defines** — there is no mechanism to attach
guidance to names you do not own. A search is assembled across every archive in
the resolved dependency set, each contributing its own.

### In the compiler

The standard library is not a resolved `.cja` dependency — its source is
embedded in the compiler binary — so its skills are embedded too, as a single
zstd-compressed corpus decompressed on first access. Alongside them sit two
pseudo-libraries and a router that have no home in the library/package model:

| Domain | Count | Covers |
|---|---|---|
| `cajeta.language` | 9 | overview, ownership, types & allocation, classes & multiple inheritance, templates, lambdas, concurrency, errors, annotations |
| `cajeta.toolchain` | 7 | driver overview, project layout & `cajeta.json`, tasks, build, `jit-run`, AOT compile, testing, skill discovery |
| `cajeta.stdlib` | 1 | the root router — a task → package table over the whole stdlib |
| stdlib packages | 164 | `io` 17, `collection` 14, `reflect` 13, `math` 13, `lang` 13, `xpu` 12, `ifx` 12, `concurrent` 12, `codec` 12, `time` 11, `hash` 10, `process` 7, `search` 6, `wire` 4, `error` 4, `nucleo` 2, `gfx` 2 |

Authoring homes: `runtime/src/cajeta/<pkg>/skills/*.md` for the stdlib,
`runtime/skills/<domain>/*.md` for the domains. Both are globbed into the
embedded corpus at compiler-build time — edit a skill, rebuild, and the compiler
serves the new payload with no packaging step.

The discovery context is **always** seeded with these before any lockfile
packages, so skill discovery works with no project, no lockfile, no dependencies,
and no network.

## URIs

```
cja-skill://<library>@<version>/<skill-id>
e.g.  cja-skill://cajeta.io@1.4.2/file-open
```

The URI is *logical*, not a network location: it names a library coordinate, a
version, and a skill id, and is resolved against the locally-resolved archive
via the lockfile. Authors write the `id` and `applies-to`; the
`<library>@<version>` prefix is supplied at resolution.

The version is the **resolved** version, not a declared range, which makes a URI
a stable cross-machine identity — and therefore a valid cache key. An agent that
already holds the payload for a URI skips the fetch entirely.

## Matching

### Hierarchical and prefix-inclusive

A search for a name returns the union of: skills bound **exactly** to it; skills
bound to any **descendant** of it (so a package query surfaces its classes' and
methods' skills); and the nearest **ancestor overview** (so a method query also
surfaces the class or package overview). Results are deduplicated by URI and
ranked closest-first, each tagged `exact` | `descendant` | `ancestorOverview`.
One query surfaces the neighborhood before the agent writes a line.

### Fuzzy

Search is typo-tolerant by design, not as a nicety — `cajeta/io/Fiel.open`
resolves `cajeta/io/File.open`. Matching runs over both canonical-name keys and
skill **titles**. Names are tokenized on their structural boundaries (`/` between
packages, `.` for members) and matched per segment, so a typo in one segment does
not force a rematch of the whole string; titles are matched token-wise. The
metric is Damerau–Levenshtein (adjacent transpositions cost 1) behind a
trigram-similarity prefilter, under a length-scaled threshold — below threshold
is no match, not garbage. Ranking is by match quality, then hierarchy proximity:
exact outranks fuzzy, a name match outranks a title match at equal distance,
closer edits outrank farther ones. Matching is fully deterministic, and every
result reports the **matched canonical name**, so the caller can see what its
query resolved to. `--exact` / `exact: true` turns the fuzz off.

This is spelling-tolerant matching, **not** semantic search.

### List is not fuzzy

`listSkills` browses rather than resolves: it filters, it does not guess. An
optional `scope` (a library or package name, slash form) restricts to that
subtree, prefix-inclusive. Ordering is deterministic.

### Versions in a diamond

A build may resolve two versions of the same library side by side. Versioning is
handled at the request and URI levels, never in the skill entry:

- **Get is always exact** — the URI carries the version, so it resolves to
  exactly one archive.
- **`version` given** → results restricted to that resolved version.
- **`version` omitted, one version resolved** → that version.
- **`version` omitted, several resolved** → matches for *every* resolved version,
  each URI version-tagged. No silent pick.
- **`from: <module>`** infers the version the *asking module* sees, so a caller
  in a diamond need not know it. An explicit `version` overrides `from`.

## Publishing a library with skills

1. Write `skills/*.md` at your package root, one file per skill, following the
   level rules above.
2. `cajeta build` — the toolchain validates each skill, builds
   `skills/index.json`, and embeds both into the `.cja`.
3. `cajeta publish` — the skills ship as part of the release to
   [olla.cajeta.dev](https://olla.cajeta.dev).
4. A consumer who resolves your library gets your guidance offline, pinned to
   the exact version they resolved, discoverable by `searchSkills` against any
   name your library defines.

### Review checklist

- Can an agent tell from the first lines whether this is the right code for its
  task?
- Is there a routing table, and does it include **negative** rows ("X is not
  provided here — do Z instead")?
- Is ownership stated at every boundary the value crosses — who transfers, who
  borrows, who frees?
- Is the example real, idiomatic, complete with imports, and ideally backed by a
  passing test?
- Does every fact live at exactly one level, with links instead of copies?
- Would removing this skill cause failure class (a), (b), (c), or (d)? If not,
  it does not belong in the catalog.

## Reading skills yourself

```sh
cajeta search-skill cajeta/collection/ArrayList     # ranked URIs + matched names
cajeta list-skills cajeta/language                  # browse a subtree
cajeta get-skills cja-skill://cajeta.language@1.0/language-ownership
```

Add `--json` for the machine shape, `--exact` to disable fuzzy matching, and
`--version` / `--from` to pin version resolution. `get-skills` takes a
comma-delimited URI list and fetches in one call. The same three operations are
served to agents over MCP by [`cajeta compiler-mcp`](CompilerMcp.md), from the
same in-process cores — identical results are a test.
