# 01 — Installation

Two ways in: `cvm` (the toolchain manager) or building from source.

## With cvm

`cvm` installs and switches Cajeta toolchains the way `rustup` does for Rust.
It lives in [`tools/cvm`](../../tools/cvm/):

```bash
cvm install latest      # download, verify checksum, install, activate
cvm default <version>   # switch the active toolchain
cvm which               # show the cvm home + active shim
cvm doctor              # diagnose the environment cvm sees
```

Toolchains install under `~/.cajeta/versions/<version>/`; the active `cajeta`
shim goes on your `PATH`. `cvm self update` updates cvm itself.

## From source

```bash
git clone https://github.com/jklappenbach/cajeta.git
cd cajeta
./setup.sh     # toolchain prerequisites (LLVM, ANTLR, CMake, Ninja)
./build.sh     # builds the compiler
```

The binary lands at `build/src/cajeta`. On Windows use `setup.cmd` and
`build.cmd`.

## Verify

```bash
$ cajeta --version
cajeta 0.8.0 (8190ee59)
```

Any version string means you're installed. Platform packages (`.deb`, `.rpm`,
`.msi`, macOS bundles) are on the roadmap; today it's cvm or source.

Next: [Kick the tires](02-kick-the-tires.md).
