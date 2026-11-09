# cajeta.process

This section measures spawning children and piping through them.

| Page | |
|---|---|
| [Command](Command.md) | Spawning a child |
| [Process](Process.md) | A running child and its pipes |

## Standing

Geometric mean 0.7x: a win over 2 benchmarks. Spawn is at parity with Rust and posix_spawn; piping 100 MB is 2x faster than Rust's std::process.
