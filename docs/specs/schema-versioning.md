# Schema versioning policy

This document defines how the cajeta manifest, lockfile, and capability
vocabulary evolve across versions. The goal is one rule: **a project that
parses today keeps parsing tomorrow unless its `schema-version` opts in.**

## The artifacts

| Artifact          | Schema doc                       | Authoritative loader                |
|-------------------|----------------------------------|-------------------------------------|
| `cajeta.json`     | `manifest-v1.json`               | `src/cajeta/buildtool/Manifest.cpp` |
| `cajeta.lock`     | `lockfile-v1.json`               | `src/cajeta/buildtool/Lockfile.cpp` |
| Action catalog    | `action-catalog-v1.json`         | `src/cajeta/buildtool/actions/`     |
| Capabilities      | `capabilities-v1.json`           | `src/cajeta/buildtool/Sandbox.{h,cpp}` |
| Repository wire   | `repository-protocol-v1.md`      | `src/cajeta/buildtool/repo/`        |
| Plugin protocol   | `extension-api-v1.md`            | `src/cajeta/buildtool/PluginRuntime.{h,cpp}` |
| Toolchain registry| `toolchain-registry-v1.md`       | `src/cajeta/buildtool/Toolchain.{h,cpp}` |
| Skill index       | `skill-index-v1` (in-archive)    | `src/cajeta/buildtool/skill/SkillIndex.cpp` |

Each shipped artifact carries a major version in its `$id` / title. v1 is
the first stable line.

## The `schema-version` field

Every cajeta.json _may_ include a top-level `schema-version` string:

```json
{
  "schema-version": "manifest-v1",
  "details": {"name": "...", "version": "..."}
}
```

Defaults:

- Absent `schema-version` is treated as `manifest-v1` for any project that
  the toolchain knows how to parse. (The toolchain's "knows how to parse"
  surface is itself v1 for the foreseeable future — see "When v2 ships"
  below.)
- A loader that encounters a `schema-version` it doesn't understand emits a
  citation-style error naming both the requested version and the supported
  set, so the user can either upgrade the toolchain or rewrite to a
  supported version.

The same `schema-version` rule applies to plugin manifests and to
melt manifests; both share the cajeta.json shape.

## Versioning rules

### Major (breaking)

A major bump (`manifest-v1` → `manifest-v2`) is reserved for:

- Removing a previously-required field.
- Removing or renaming a top-level block.
- Changing the meaning of an existing field (e.g. `settings.dependencies`
  changing its value shape).
- Reducing the capability vocabulary (`capabilities-v1` → `capabilities-v2`
  with `network` removed would be a major bump even though "fewer caps" is
  smaller, because consumers may have allowlisted it).

A major-version document is only loaded by a toolchain that opts in. v1
toolchains see `manifest-v2` as "unsupported" and refuse to load.

### Minor (compatible)

A minor bump (`manifest-v1.0` → `manifest-v1.1`) covers:

- Adding a new optional top-level block (the v1 loader doesn't know about
  it; the strict allowed-block check rejects it; this is therefore actually
  a major bump unless the v1 loader is updated to tolerate the new block).
- Adding new fields under an existing block where the loader uses a strict
  "unknown field is an error" check (also actually a major bump unless the
  v1 loader is updated).

In practice, cajeta's strict-loader policy means most additive changes
require a coordinated update: bump the v1 line minor version, ship a loader
that knows about the new field, and require the user to install at least
that toolchain. Strictness is intentional — silent drift between manifest
and tool is what `schema-version` exists to prevent.

### Patch (clarification only)

A patch bump (`manifest-v1.0.0` → `manifest-v1.0.1`) is reserved for
clarifying language in the document, fixing typos, or tightening a
description without changing what loaders accept.

## Capability vocabulary special case

Adding a capability to `capabilities-v1.json` is a minor bump per the
"Open decision: capability minor-version drift" item in
`plans/buildtool/build-tool-plan.md`. The lean is: yes, treat any capability addition
as a minor bump. The reasoning: existing `plugins-allowed-capabilities`
allowlists keep working; the new capability simply isn't granted to old
allowlists. New allowlists can opt in.

Removing a capability is a major bump.

## When v2 ships

A v2 line is on the table when:

- A wire-incompatible change to the repository protocol is needed.
- The plugin protocol grows a structurally new envelope kind that older
  hosts can't ignore gracefully.
- The capability vocabulary loses a meaningful entry.

When v2 ships, both v1 and v2 loaders coexist in the toolchain for at least
one major-version cycle of the toolchain itself, so consumers have a
deprecation window. The toolchain compatibility window
(`cajeta-lang-version` and the N±k policy) is the time bound.

## Toolchain implications

The toolchain version pin in `settings.toolchain` is the user's lever for
"this project is locked to a schema version I've validated." A team that
bumps `manifest-v1` → `manifest-v2` in their project must also bump the
toolchain pin in lockstep, so CI on machines without the new toolchain
fails-fast with a clear "update your toolchain" error rather than a
manifest-parse failure.

## Today's status

- v1 is the only published line for every artifact in the table above.
- v2 is not currently planned for any artifact.
- `schema-version` is optional today and defaults to v1 if absent.
- The strict allowed-block / unknown-field error policy in
  `Manifest.cpp::loadManifestString` is what enforces the "no silent drift"
  guarantee.
