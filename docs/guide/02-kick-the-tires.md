# 02 — Kick the tires

One binary does everything. `cajeta <subcommand>` runs a built-in command;
anything else is looked up as a task in your project's `cajeta.json`.

## The commands you'll use daily

| Command | What it does |
|---|---|
| `cajeta init [type] [dir]` | Scaffold a project (`init --list` shows the archetypes) |
| `cajeta build` | Build the current project |
| `cajeta test` | Run the project's tests |
| `cajeta tasks` | List the project's tasks with descriptions |
| `cajeta clean` | Wipe build outputs |
| `cajeta add / remove / upgrade / pin` | Manage dependencies in the manifest |
| `cajeta install` | Publish this project's library into the local repository |
| `cajeta publish` | Publish to a remote repository |
| `cajeta info / show` | Inspect the project / a dependency |

## The rest of the surface

- **Archives** — `cajeta archive <cmd>` creates, inspects, and signs `.cja`
  archives.
- **Docs** — `cajeta doc <source-root>` generates API documentation
  (`--emit-model-json` for tooling).
- **Skills** — `cajeta search-skill`, `list-skills`, `get-skills` find
  implementation guidance shipped inside resolved dependencies.
- **Quality** — `cajeta coverage`, `verify`, `verify-reproducible`, `trust`.
- **Workspace** — `cajeta workspace` / `members` for multi-project trees.
- **Toolchain** — `cajeta toolchain`, `cajeta which`, `cajeta sandbox-info`.
- **IDE & debugging** — `cajeta ide install` (IntelliJ plugin),
  `cajeta dap` (Debug Adapter Protocol server), `cajeta jit-run` (compile +
  run an entry point without a project).

## Task fallthrough

Unknown subcommands resolve against `cajeta.json` tasks. That's how
`cajeta run` works in projects that define a `run` task — it's a task, not a
built-in. `cajeta tasks` always shows what your project offers:

```bash
$ cajeta tasks
  build    Local development build
  clean    Wipe build outputs
  lint     Run native + plugin lint actions
  release  Release build + ship
  test     Unit tests with coverage instrumentation
```

Next: [Your first project](03-your-first-project.md).
