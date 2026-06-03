# Tour: build your first cajeta package

This walks you from "I just installed cajeta" to "I built, tested, and
packaged my project." It assumes the cajeta toolchain is on your `PATH`
(via `cajeta toolchain default <distribution>:<version>` or a system
install). For language syntax see the language tour under
`cajeta-docs/`; this tour is the **build-tool** introduction.

## 1. Initialize a project

```sh
mkdir hello-cajeta && cd hello-cajeta
cajeta init basic
```

`cajeta init` writes a starter `cajeta.json` (from
`samples/buildtool/basic/`) plus a `src/main/cajeta/` source tree. Other
archetypes available: `workspace`, `multi-binary`, `melt`.

Open the generated `cajeta.json`. You'll see six top-level blocks:

| Block        | What it carries                                           |
|--------------|-----------------------------------------------------------|
| `details`    | package identity (`name`, `version`, `authors`, license)  |
| `properties` | user-defined `${PROPERTY}` substitution values            |
| `settings`   | deps, repos, plugins, build defaults, capability allowlist|
| `actions`    | reusable action-parameter presets (empty by default)      |
| `plugins`    | resolved plugin pin slot — populated by `cajeta build`    |
| `tasks`      | what you actually run with `cajeta <task>`                |

There's no fixed lifecycle. The starter ships four tasks (`build`, `test`,
`release`, `clean`, plus `lint`) that your project owns from day one —
edit, rename, or delete them as the project shapes.

## 2. Build

```sh
cajeta build
```

What happens:

1. The build tool loads `cajeta.json`, validates the schema, then composes
   the resolved property set (CLI `-P` > env `CAJETA_PROPERTY_*` > manifest).
2. It resolves dependencies — for the starter that means fetching
   `cajeta.lang` and `cajeta.lang.io` from the configured central
   repository. The first build writes `cajeta.lock` capturing the resolved
   versions + manifest checksum.
3. It runs the `build` task, which calls the `build` native action with
   `flavor: debug, profile: dev`.
4. Output lands at `build/exe/<name>` (or `build/archive/<name>-<version>.cja`
   when there's no entry method).

The artifact path is published as `${art.path}` so downstream actions or
tasks can chain it.

## 3. Iterate

The IR cache means re-running `cajeta build` after editing one source
file recompiles only that file plus its dependents. The cache lives at
`.cajeta/cache/ir/<discriminator>/` and is keyed on
`(compiler-version, flags, source-digest)` so flag changes invalidate the
right set automatically.

Touch a source file, run `cajeta build` again, watch the second build be
fast.

## 4. Test

```sh
cajeta test
```

The starter wires the first-party `cajeta.coverage` plugin into the `test`
task: instrument → run → emit reports under `build/reports/`. Coverage is
gated by `min: 80` (overall) + `min-per-file: 50`; a violation fails the
task with a citation naming the worst-N files.

## 5. Lint

```sh
cajeta lint
```

The starter `lint` task runs:

- the language-level lint pass (via `cajeta lint`), and
- the first-party `cajeta.lint.security` plugin (banned imports +
  secret patterns).

Findings stream into `ActionResult.findings` and surface in the unified
console output.

## 6. Add a dependency

```sh
cajeta add com.example.shared
cajeta build
```

`cajeta add` rewrites `settings.dependencies` and re-resolves the
lockfile. Use `cajeta upgrade <dep>` to bump versions; the tool prompts
when a capability changes (new capabilities are minor-version events but
worth a beat of human attention).

## 7. Package + publish

The `release` task in the starter is intentionally a stub — replace the
`exec` call with the real pipeline when you're ready:

```jsonc
"release": {
  "actions": [
    { "action": "build",   "flavor": "release", "id": "art" },
    { "action": "sign",    "input":  "${art.path}",
                           "key-env": "CAJETA_SIGN_KEY",
                           "key-id":  "you@example.com",
                           "id":      "sig" },
    { "action": "package", "input":  "${art.path}",
                           "format": "container",
                           "tag":    "${details.name}:${details.version}",
                           "id":     "img" },
    { "action": "publish", "archive": "${art.path}",
                           "signature": "${sig.signature-path}",
                           "key-id":    "${sig.key-id}",
                           "attestation": true,
                           "url":       "https://repo.cajeta.org/v2/publish" }
  ]
}
```

Then:

```sh
cajeta release
```

The pipeline builds, signs (ed25519, key from `CAJETA_SIGN_KEY`),
packages as an OCI container, and publishes the archive + signature +
SLSA v1 provenance to the registry.

Consumer-side verification:

```sh
cajeta install foo-1.2.3.cja --require-signature --require-attestation
```

## 8. Reproducible builds

Two machines, same source, same lockfile, same toolchain pin → byte-
identical `.cja`. The toolchain emits `SOURCE_DATE_EPOCH`,
`--debug-prefix-map=<projectRoot>=cajeta:`, and a content-bound seed; the
sandbox is hermetic (no network on `build`, no host-path leaks). Verify:

```sh
cajeta verify-reproducible build-a/foo-1.2.3.cja build-b/foo-1.2.3.cja
```

## 9. Pin your toolchain

```sh
cajeta toolchain pin 1.0.3
git add cajeta.json .cajeta-toolchain
```

Anyone cloning your repo who runs `cajeta build` from a different
toolchain version will see the launcher dispatch transparently to 1.0.3
(or, with `fetch: auto`, download + install + dispatch on first build).
End of "works on my machine" toolchain mismatches.

## Where to go next

- `cajeta-docs/BuildTool.md` — full spec.
- `cajeta-docs/specs/manifest-v1.json` — formal manifest schema.
- `cajeta-docs/specs/action-catalog-v1.json` — action-by-action contract.
- `cajeta-docs/specs/extension-api-v1.md` — write your own plugin.
- `cajeta-docs/specs/repository-protocol-v1.md` — host your own registry.
- `cajeta-docs/specs/toolchain-registry-v1.md` — host your own toolchain
  distribution.
