# `library` archetype

The starter written by `cajeta init library [<dir>]`. Use it for a **library**
project — one that compiles to a reusable `.cja` archive other projects depend
on, rather than a native executable.

What `cajeta init library` writes (the embedded files):

```
cajeta.json                                          # library manifest (no entry-method)
src/main/cajeta/com/example/library/Greeter.cajeta   # sample public API — NO main
```

## How a library differs from `basic`

The **only** structural difference is the manifest: a library has **no
`settings.build.entry-method`**. With an entry point the `build` action emits an
executable; with none it emits a `.cja` library archive. There is no `main`.

## Next steps

1. Set `details.name` (reverse-DNS) and rename the `src/main/cajeta/...` package
   to match.
2. Replace `Greeter` with your public API.
3. Add runtime deps under `settings.dependencies`, test-only deps under
   `dev-dependencies`.
4. `cajeta build` → `.cja`; `cajeta test`; `cajeta release`.

See `docs/buildtool/LibraryProjectType.md` for the full specification and
`plans/library-archetype-plan.md` for the implementation plan.
