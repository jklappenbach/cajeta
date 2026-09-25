# cvm-installer — spec

## 1. Definition

The compiler reaches a user four ways: an archive, a bare binary, a native
installer, and `cvm`. cvm itself reaches a user exactly one way, as a bare
binary that arrives without its executable bit. This spec adds the native
installer channel for cvm, on every platform the release matrix builds.

### 1.1 Why

Measured 2026-09-24 against the published v0.29.1 assets.

- **1.1.1** Every cvm asset downloads at mode `664`. `./cvm --version` answers
  `permission denied` until the reader runs `chmod +x`, and the README and the
  cajeta.dev home page do not mention that. An installer removes the step
  rather than documenting it.
- **1.1.2** cvm is the FIRST binary a user runs, on a machine that has nothing.
  A reader who already has `apt`, `dnf`, `msiexec` or Installer.app is better
  served by the tool they already trust than by a chmod they have to be told
  about.
- **1.1.3** The compiler already ships `.deb`, `.rpm`, `.msi` and `.pkg` from
  the same matrix. cvm has no such channel, and the asymmetry is an accident of
  how each is built rather than a decision.

### 1.2 The obstacle

The compiler's installers come from CPack, driven by the CMake project's
`install()` rules and `cmake/CPackOptions.cmake`. cvm is built by the cajeta
build tool from `tools/cvm/cajeta.json` and is not a CMake target, so no CPack
generator can see it today.

### 1.3 Scope

Packaging for the cvm binary on all four triples, its publication as release
assets, and its appearance on the README table and the cajeta.dev home page.

### 1.4 Non-goals

- **1.4.1** Changing how cvm is BUILT. The installer packages the binary the
  release already produces.
- **1.4.2** Bundling cvm into the compiler's installer. cvm exists to install
  the compiler, so the reverse dependency would invert the bootstrap.
- **1.4.3** Retiring the bare binary. It is the zero-dependency path for a
  machine with no package manager, and it stays.

## 2. What the package contains

- **2.1** When the package installs, it places exactly one executable on the
  PATH, with its executable bit set.
- **2.2** When the package installs, it adds nothing else. No libraries, no
  headers, no data, no service.
- **2.3** When the package is removed, the executable is removed and nothing
  else is left behind.
- **2.4** When both the compiler package and the cvm package are installed,
  neither owns a file the other owns.

## 3. Naming and version

- **3.1** When the package is named, it is distinct from the compiler's, so a
  package manager can hold both.
- **3.2** When the package carries a version, that version increases from one
  release to the next, because a package manager decides upgrades by ordering.
  cvm's own declared version is `0.1.0` and has not changed across releases, so
  it cannot serve as the package version unmodified.
- **3.3** When a user asks the installed cvm its version, the answer agrees
  with the version the package manager reports. Two different answers to one
  question is a bug report nobody can place.

## 4. Built by the release, on every platform

- **4.1** When a release builds cvm for a triple, it also builds that triple's
  native package, in the same leg, after the binary exists.
- **4.2** When a platform's packaging tool is absent, that platform publishes
  no cvm package and the release still succeeds. The compiler's installer steps
  are already non-fatal for the same reason.
- **4.3** When a leg publishes a cvm package, it publishes a matching digest
  beside it, as every other asset does.
- **4.4** When the package is built, it is built from the same binary the bare
  asset carries, not a second compilation.

## 5. Visible where the downloads are

- **5.1** When the README download table renders, a cvm package appears in its
  own cell, distinct from the bare cvm binary and from the compiler's
  installer.
- **5.2** When the cajeta.dev home page renders, the same holds.
- **5.3** When a triple has no cvm package, its cell reads as absent and the
  row still renders.
- **5.4** When a reader sees both a cvm package and a bare cvm binary, the page
  says which to prefer and why, since the bare binary needs `chmod +x` and the
  package does not.

## 6. Open questions

- **6.1 DECIDED 2026-09-24 by Julian: cvm's own version, bumped.** The package
  carries `details.version` from `tools/cvm/cajeta.json`, not the release tag,
  because a 1.0 cvm installs an 8.0 cajeta and the compiler's version cannot
  speak for it. Bumped 0.1.0 to 0.2.0: native installers are a real increment,
  and 1.0 would overclaim while `cvm install` has never completed end to end.
  My recommendation had been the release tag, on the grounds that a tag is
  monotonic for free. It buys that at the price of 3.3, and the disagreement is
  worse than the discipline: the container test printed a package version of
  0.29.1 beside a `cvm --version` of 0.1.0.
  The discipline this needs is now machine-enforced rather than remembered.
  `cvm --version` reads a constant in `Cvm.cajeta` and the packages read the
  manifest, nothing joined them, and `scripts/check-cvm-version.sh` fails the
  build when they drift, rejects the 0.1.0 placeholder outright, and checks the
  BUILT binary prints it, since a stale build prints the old number whatever the
  source says. What is NOT enforced is that a release bumps at all: two releases
  without a bump declare the same version and offer no upgrade.
- **6.2** Does the cvm package install to a prefix that is on PATH by default
  on each platform? `/usr/local/bin` is on Linux and macOS. Windows has no
  equivalent, so the MSI must edit PATH or install somewhere already on it.
