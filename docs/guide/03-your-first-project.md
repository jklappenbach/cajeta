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

- **`details`** holds the name, version, description, and license.
- **`settings`** holds capabilities (see [chapter 04](04-running.md)),
  dependencies, repositories, and `settings.build.entry-method`
  (`com.example.basic.Main::main`). The presence of an entry method is what
  makes this a binary project.
- **`tasks`** holds named action pipelines. The archetype ships `build`,
  `test`, `clean`, `lint`, and `release`, and you add your own. `cajeta tasks`
  lists them:

```bash
$ cajeta tasks
  build    Local development build
  clean    Wipe build outputs
  lint     Run native + plugin lint actions
  release  Release build + ship
  test     Unit tests with coverage instrumentation
```

## Project types

`cajeta init --list` shows these:

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

The `package` action turns build outputs into distributables. These formats
ship today:

| Format | Output |
|---|---|
| `obj-tree` | Per-source native `.o` tree |
| `uber-ir` | Single linked LLVM bitcode `.bc` |
| `uber-archive` | One `.cja` carrying the project and all transitive deps |
| `static-lib` / `shared-lib` | Native `.a` / `.so`/`.dylib`/`.dll` |
| `tarball` / `zip` | Compressed binary distributions |
| `container` | OCI container image |

These formats are deferred, and the build tool rejects them with a "deferred
slice" error for now: `deb`, `rpm`, `msi`, `app-bundle`, `pkg`, `dmg`,
`appimage`, `flatpak`, `snap`.

## The wider command surface

One binary does everything. Unknown subcommands fall through to your
manifest's tasks, which is how `cajeta run` works when a `run` task exists
([chapter 04](04-running.md)).

| Command | What it does |
|---|---|
| `cajeta build / test / clean` | Project tasks |
| `cajeta add / remove / upgrade / pin` | Manage dependencies |
| `cajeta install / publish` | Publish locally / remotely |
| `cajeta info / show` | Inspect the project / a dependency |
| `cajeta deps` | Print the dependency tree (text, JSON, CSV) |
| `cajeta archive <cmd>` | Create, inspect, sign `.cja` archives |
| `cajeta doc <root>` | Generate API documentation |
| `cajeta search-skill / list-skills / get-skills` | Skill discovery in dependencies |
| `cajeta coverage / verify / verify-reproducible / trust` | Quality and provenance |
| `cajeta workspace / members / toolchain / which / sandbox-info` | Environment |
| `cajeta ide / dap / jit-run` | IDE plugin, debug server, quick runs |
| `cajeta compiler-mcp` | MCP server (stdio) serving those skills to a coding agent |

`cajeta --help` shows the same list.

## Skills, for AI coding agents

The same skills the `search-skill` / `get-skills` commands return are also
served over the [Model Context Protocol](https://modelcontextprotocol.io) by
`cajeta compiler-mcp`. It is a stdio server built into the compiler, and it
exposes `searchSkills`, `listSkills`, and `getSkills`. Register it with your
agent, and the agent can look up authoritative guidance for a class, package,
or language topic before it writes code. This repo ships a `.mcp.json` that
does exactly that.

The corpus is embedded in the binary, so it works with no project, no
lockfile, and no network. More than 180 skills cover the language itself
(`cajeta.language`), the toolchain (`cajeta.toolchain`), and every stdlib
package.

Your library ships skills too. Skills are part of the `.cja` archive format,
so any library can carry them. Write them as `skills/*.md` at your package
root, and `cajeta build` validates, indexes, and embeds them beside the
bitcode. Anyone who resolves your dependency gets your guidance offline,
pinned to the version they resolved and discoverable by name.

```
mylib.cja
├── … compiled bitcode members
├── skills/index.json
├── skills/opening-files.md
└── skills/retry-policy.md
```

The format is a Markdown body under YAML frontmatter (`id`, `applies-to`,
`title`, `description`). See [Skills](../specification/mcp/Skills.md) for the
authoring levels and the review checklist, and
[CompilerMcp](../specification/mcp/CompilerMcp.md) for the server's tools and
lifecycle.

The [build tool reference](../specification/buildtool/BuildTool.md) is the
longer walk, and it covers dependencies, publishing, and uber-archives end to
end.

Next: [Running your application](04-running.md).
