# 03 — Your first project

[Chapter 02](02-kick-the-tires.md) built and ran `hello`. Now look at what
`init` actually created, and what else the toolchain can make.

## The layout

```
hello/
├── cajeta.json                          # the manifest
├── src/main/cajeta/<package>/*.cajeta   # sources, dirs mirror packages
└── build/                               # outputs (gitignored)
```

## The manifest

`cajeta.json` has three parts:

- **`details`** — name, version, description, license.
- **`settings`** — capabilities (see [chapter 04](04-running.md)),
  dependencies, repositories, and `settings.build.entry-method`
  (`com.example.basic.Main::main`) — the presence of an entry method is what
  makes this a binary project.
- **`tasks`** — named action pipelines. The archetype ships `build`, `test`,
  `clean`, `lint`, and `release`; you add your own. `cajeta tasks` lists them:

```bash
$ cajeta tasks
  build    Local development build
  clean    Wipe build outputs
  lint     Run native + plugin lint actions
  release  Release build + ship
  test     Unit tests with coverage instrumentation
```

## Project types

`cajeta init --list`:

| Archetype | What you get |
|---|---|
| `basic` | A binary: entry method, builds to `build/exe/<name>` |
| `library` | No entry method: builds to a `.cja` archive other projects depend on |
| `workspace` | A multi-member tree with shared configuration |
| `multi-binary` | One project, several entry points / binaries |
| `melt` | A curated bundle of dependency versions and properties (a BOM) |

A library is the same project minus the entry method:

```bash
$ cajeta init library greetlib && cd greetlib
$ cajeta build
# → build/archive/greetlib-0.1.0.cja
```

`cajeta install` publishes the `.cja` into your local repository so sibling
projects can depend on it before it's ever published remotely.

## Every artifact kind

The `package` action turns build outputs into distributables. Shipped today:

| Format | Output |
|---|---|
| `obj-tree` | Per-source native `.o` tree |
| `uber-ir` | Single linked LLVM bitcode `.bc` |
| `uber-archive` | One `.cja` carrying the project and all transitive deps |
| `static-lib` / `shared-lib` | Native `.a` / `.so`/`.dylib`/`.dll` |
| `tarball` / `zip` | Compressed binary distributions |
| `container` | OCI container image |

Deferred (the build tool rejects these with a "deferred slice" error for now):
`deb`, `rpm`, `msi`, `app-bundle`, `pkg`, `dmg`, `appimage`, `flatpak`, `snap`.

## The wider command surface

One binary does everything; unknown subcommands fall through to your
manifest's tasks (that's how `cajeta run` works when a `run` task exists —
[chapter 04](04-running.md)).

| Command | What it does |
|---|---|
| `cajeta build / test / clean` | Project tasks |
| `cajeta add / remove / upgrade / pin` | Manage dependencies |
| `cajeta install / publish` | Publish locally / remotely |
| `cajeta info / show` | Inspect the project / a dependency |
| `cajeta archive <cmd>` | Create, inspect, sign `.cja` archives |
| `cajeta doc <root>` | Generate API documentation |
| `cajeta search-skill / list-skills / get-skills` | Skill discovery in dependencies |
| `cajeta coverage / verify / verify-reproducible / trust` | Quality and provenance |
| `cajeta workspace / members / toolchain / which / sandbox-info` | Environment |
| `cajeta ide / dap / jit-run` | IDE plugin, debug server, quick runs |

`cajeta --help` shows the same list.

For the longer walk — dependencies, publishing, uber-archives end to end — see
[Build your first package](../tour-build-your-first-package.md) and the
[build tool reference](../specification/buildtool/BuildTool.md).

Next: [Running your application](04-running.md).
