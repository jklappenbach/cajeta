# `melt` — curated version-set / BOM package

A melt-only project. Cajeta's name for what Spring users call
a BOM, Maven users call dependency management, and Gradle users
call a version catalog. The package's purpose is to **export
curated configuration** rather than executable code.

## Layout

```
melt/
├── cajeta.json         # has "melt" top-level block; NO source, NO tasks
└── run.sh
```

That's it. A melt is consumed by reference; publishing emits a
`.cja` containing only the manifest.

## What this sample demonstrates

- The `melt` top-level manifest block. It carries:
  - `dependencies` — version constraints consumers inherit
  - `properties` — properties consumers see after importing
  - `actions` — action presets consumers can invoke
  - `repositories` — added to the consumer's resolution list
  - `melts` — transitive melt imports (this sample's is empty)
- No `tasks` block (melts aren't built).
- No `src/` (melts have no code).
- Why the name "melt"? Cajeta is caramel; a melt is how the
  ingredients fuse into one cohesive consumable unit.

## What today's run.sh exercises

Melt parsing + import resolution lands in Phase 6c. Until then
the `run.sh` here verifies:

- The manifest parses cleanly (a melt's structure validates).
- `cajeta info` prints the details + the (empty) standard
  blocks; the `melt` block is currently unmodeled by the
  validator so it's stored as an extra block. This will tighten
  to first-class with Phase 6c.

## Running

```
./run.sh
```

## How a CONSUMER imports this melt

```jsonc
// consumer/cajeta.json
{
    "details": { "name": "com.example.my-app", "version": "0.1.0" },
    "settings": {
        "melts": [
            "com.example.platform-melt@2024.1.0"
        ],
        "dependencies": {
            // "*" means "use whatever the melt says" (1.2.5 from
            // this melt's curated table).
            "dev.cajeta.http": "*",
            "cajeta.lang":        "*",
            // Explicit version overrides the melt.
            "vendor.special":     "3.2.1"
        }
    }
}
```
