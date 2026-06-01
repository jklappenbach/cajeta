# Installer Plan

Governs the work required to ship **native OS installers + distribution
channels** for the cajeta compiler — **plus the IDE plugin** — as part of the
release process. The task is **done** when a tagged release publishes
installable artifacts for every supported channel, the IDE plugin installs via
`cajeta ide install` and auto-publishes to the JetBrains Marketplace, the GitHub
release workflow produces everything automatically, and each artifact has been
install-tested in a clean environment.

> **Tracking:** every line item is a checkbox; complete when every box in
> [§13 Definition of Done](#13-definition-of-done) is checked.
>
> **Resolved decisions:** D1 (Arch → AUR), D2 (self-contained), D3 (macOS `.pkg`),
> D4 (RISC-V deferred), D5 (paths → §2), D8 (plugin via `cajeta ide install`),
> D6 (metadata), D7 (stay MinGW), D9 (plugin version synced), D10 (Marketplace
> in scope), D11 (IDEA only), D12 (toolchain manager phased; written in Cajeta).
> **All decisions resolved.**

---

## 0. Current state (baseline)

- `.github/workflows/release.yml` cross-builds `cajeta` for 4 targets
  (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `aarch64-apple-darwin`,
  `x86_64-w64-mingw32`) and **hand-stages** `bin/cajeta` + `VERSION` +
  `README.md` + `LICENSE` into a `.tar.gz` / `.zip`.
- **No `install()` rules** and **no `CPack`** anywhere.
- `RELEASING.md` documents a richer artifact shape (`lib/cajeta-stdlib.cja`) the
  workflow does **not** produce yet.
- The compiler dynamically links `libLLVM`, `antlr4-runtime`, `zstd`, `glog`,
  `OpenSSL::Crypto`, `libstdc++`. The **stdlib is embedded in the binary**.
- The CLI already has subcommand dispatch (`cajeta archive|jit-run|dap`,
  `src/main.cpp:147`) designed to grow, and a comprehensive `printUsage`
  (`src/main.cpp:46`) — but `--help` does **not** list the subcommands.
- **IDE plugin:** only `ide-plugins/idea` (no VS Code). Gradle + IntelliJ
  Platform Plugin 2.x, JDK 21, `pluginVersion = 0.1.0`, compat `242 … 261.*`.
  Not built by the release workflow, not published anywhere; installs have been
  manual. Gradle already provides `buildPlugin`/`verifyPlugin`/`signPlugin`/
  `publishPlugin`.

---

## 1. Decisions

- [x] **D1 — Packaging toolchain → CPack for deb/rpm/msi/macOS-pkg; Arch via AUR
      `PKGBUILD`.** No pacman CPack generator; user doesn't use Arch. The
      self-contained binary already runs on Arch via the tarball; the idiomatic
      add-on is a maintained AUR `PKGBUILD`. No `fpm`.
- [x] **D2 — Linking model → self-contained (static-link the heavy deps).**
      Static: LLVM components, `antlr4-runtime` (`-DANTLR4CPP_STATIC` already
      set), `zstd`, `glog`, `OpenSSL` crypto, lld. Dynamic (the only declared
      Linux deps): base system runtime — `libc6`, `libstdc++6`, `libgcc-s1`,
      `pthread`. Windows reaches the same end by **bundling MinGW DLLs** in the
      MSI (the monolithic `libLLVM.dll` + MinGW don't statically link cleanly).
  - [ ] Confirm static libs exist on each runner; where a distro is shared-only,
        add the static package or build that dep static. Verify `ldd`/`otool -L`
        shows nothing beyond the base runtime.
- [x] **D3 — macOS installer → `.pkg`** (`cpack -G productbuild`), signed +
      notarized for "latest". `aarch64-apple-darwin` only; Intel = from source.
- [x] **D4 — RISC-V → DEFERRED (backlog/TODO).** No GitHub-hosted way to
      build-and-**test** a release for RISC-V (no native runner; we won't ship a
      release artifact we can't test). Kept as a documented future cell — not
      built, not gated. Revisit when a native runner or trusted QEMU test rig
      exists.
- [x] **D5 — Install paths → see [§2 Distribution strategy](#2-distribution-strategy--install-paths).**
      Principle (per user): keep the install in **one place** (home for local,
      a system dir for system-wide) and **symlink into PATH**. Concrete paths
      per OS × channel are enumerated in §2. Mechanics (CMake `install()` +
      symlink rules) tracked in §3/§5/§6.
- [x] **D6 — Package metadata → CONFIRMED.** Maintainer
      `Julian Klappenbach <jklappenbach@gmail.com>`, homepage
      `https://github.com/jklappenbach/cajeta`, SPDX id from `LICENSE`,
      description/summary from `README`, section/group `devel`.
- [x] **D7 — Windows toolchain → CONFIRMED: stay MinGW/MSYS2**
      (`x86_64-w64-mingw32`); the MSI bundles the MinGW DLLs (§5). No MSVC switch.
- [x] **D8 — Plugin install → the `cajeta` app installs it.** `cajeta ide
      install` is the cross-platform mechanism for any env running IDEA (Linux's
      only path; offered post-install on Win/Mac). The **plugin zip is embedded
      in the binary** (like the stdlib), so it works from any distribution form.
- [x] **D9 — Plugin version → SYNCED to repo `VERSION`** (for now). Release-time
      step writes `VERSION` into the plugin's `pluginVersion`; one version string
      everywhere. (Revisit if IDE-compat cadence ever diverges from the
      compiler.)
- [x] **D10 — Marketplace → in scope, auto-published** on production tags
      (`buildPlugin → signPlugin → publishPlugin`), after a one-time manual first
      upload. Needs vendor account + `PUBLISH_TOKEN` + signing cert/key (secrets).
- [x] **D11 — IDEs → IDEA only** for this release. No VS Code / other plugin
      scaffolded now.
- [x] **D12 — First-party toolchain manager → PHASED, WRITTEN IN CAJETA,
      VERSION-INDEPENDENT, and the cross-channel package presence.**
      **`cvm` (Cajeta Version Manager)** is a separate Cajeta-language app the
      compiler builds `--emit=exe` (dogfooding; source `tools/cvm/*.cajeta`).
  - **Version-independent by contract (keystone):** cvm bakes in **no** knowledge
        of any cajeta version. It (1) resolves a selector (`latest`/`8.0`/channel)
        against a **stable release manifest** at a well-known URL
        (`cajeta.dev/dist/index.json` or the GitHub releases API; the manifest has
        its own `schemaVersion` so a far-future manifest is rejected gracefully,
        not mis-parsed), (2) downloads the self-contained binary for the host
        triple + verifies checksum, (3) installs to `~/.cajeta/versions/<ver>/` and
        repoints the active shim, (4) gets out of the way. So a **1.0 cvm installs
        an 8.0 cajeta**, unchanged. (D2 self-containedness is the enabler —
        "download and run, no dep resolution.") Mirrors rustup ↔ channel manifests.
  - **Distribution = every package ecosystem + the shim** (see §2 Tier 1):
        `brew`/`apt`/`dnf`/AUR/`winget` carry the evergreen cvm; the bootstrap
        shim (`curl …sh.cajeta.dev | sh` / `irm …win.cajeta.dev | iex`) covers
        brewless/repoless. cvm being evergreen makes distro lag a non-issue — the
        argument for putting it everywhere.
  - **Self-update defers to the installer:** cvm detects its install method by its
        own exe path (under a PM prefix → package-managed; under `~/.cajeta/bin` →
        shim/self-managed). Package-managed → `cvm self update` disabled, points at
        `brew upgrade cvm` / `apt upgrade cvm` / …. Only the PM owns the cvm
        *binary*; cvm still owns all `~/.cajeta` toolchain state regardless.
  - **Reconciliation with a system-wide cajeta** (never touches the other
        installer's files): detect it, then offer **coexist** (default,
        non-destructive — register the system binary as a pinned `system`
        toolchain, manage other versions in `~/.cajeta`, PATH-prepend so the
        cvm-active version wins, `cvm default system` to switch back) **or
        takeover** (advise `apt remove cajeta` etc. so `~/.cajeta` is sole source).
        **Windows caveat:** system PATH precedes user PATH, so user-scope cvm can't
        shadow a `Program Files` install — there cvm leans toward advising takeover
        rather than coexist. `cvm which`/`cvm doctor` shows every `cajeta` on PATH
        and who owns each.
  - [ ] **Phase 1:** shim + minimal `cvm` (resolve `latest` from the manifest,
        download, install to `~/.cajeta`, wire PATH; install-method detection;
        coexist/takeover reconciliation). Built by the release workflow with the
        just-built compiler, shipped per target. No multi-version switching yet.
  - [ ] **Phase 2:** full manager — `~/.cajeta/versions/<ver>/`, `cvm self update`
        (shim installs only), `cvm toolchain install/default`, `cvm which`/`doctor`,
        per-project `cajeta-version` pin; cvm packages in each ecosystem
        (`cvm.rb` tap formula, AUR, apt/dnf repo, winget).
  - [ ] **Bonus:** compiling `cvm` with `--emit=exe` is a real-world integration
        test of the compiler's exe emission on every target.

---

## 2. Distribution strategy & install paths

**Enabler:** cajeta is one self-contained binary (D2) with stdlib + IDE plugin
embedded, so every channel is "place one executable + put it on PATH." Two
philosophies, both supported:

- **Direct single-version install** — download an OS-native cajeta installer and
  let the OS own one version. No hosted repo; for pinned / offline / no-manager /
  CI use.
- **Managed multi-version** — install **cvm** (the version manager) once; it
  fetches and switches cajeta toolchains under `~/.cajeta`. The recommended path
  for staying current, since distro repos inevitably lag.

**Channel assignment (resolved):** the package ecosystems (apt/dnf/AUR/winget/
brew) carry **cvm**, not `cajeta` — cvm is evergreen (version-independent, see
D12), so a single low-maintenance package per ecosystem covers "get latest"
forever. `cajeta` itself ships as **Tier-0 direct-download installers**; we do
**not** stand up hosted `cajeta` apt/dnf repos (they'd lag, and cvm is the answer
to lag).

### Tier 0 — Direct release artifacts (GitHub Releases)
Produced by the release workflow; the substrate every other channel pulls from:
- **cajeta installers** (single-version, install directly — `sudo apt install
  ./x.deb`, `dnf install ./x.rpm`, `installer -pkg`, `msiexec /i`): `.deb` /
  `.rpm` (per arch), `.msi` + `.zip` (Windows), `.pkg` + `.tar.gz` (macOS),
  `.tar.gz` (Linux generic).
- **cvm binaries** per target (the version manager itself).
- **IDE plugin** `cajeta-idea-<ver>.zip`.

### Tier 1 — `cvm`, the cross-channel version manager (recommended) — D12
The single managed presence in every package ecosystem. Because cvm is
**version-independent** (a 1.0 cvm installs an 8.0 cajeta — D12), shipping it in a
repo is low-maintenance and a stale packaged cvm still fetches the latest cajeta.

| Install cvm via | Command | Updated via |
|---|---|---|
| **Homebrew** (mac/Linux) | `brew install jklappenbach/tap/cvm` | `brew upgrade cvm` |
| **apt** (Debian/Ubuntu) | `apt install cvm` (hosted repo) | `apt upgrade cvm` |
| **dnf** (Fedora/RHEL) | `dnf install cvm` | `dnf upgrade cvm` |
| **AUR** (Arch) | `yay -S cvm` | AUR |
| **winget** (Windows) | `winget install cvm` | `winget upgrade` |
| **shim** (brewless/repoless) | `curl …sh.cajeta.dev \| sh` / `irm …win.cajeta.dev \| iex` | `cvm self update` |

cvm is **written in Cajeta**, compiled `--emit=exe` (dogfooding; source
`tools/cvm/*.cajeta`, built by the release workflow with the just-built
compiler). It manages cajeta under `~/.cajeta` (honoring `$CAJETA_HOME` /
`$XDG_DATA_HOME`):
- `~/.cajeta/bin/cvm` — the manager (on PATH **only** when shim-installed;
  PM-installed cvm lives in the PM's own prefix)
- `~/.cajeta/bin/cajeta` — shim → the active toolchain (on PATH)
- `~/.cajeta/versions/<version>/cajeta` — each installed toolchain
- `~/.cajeta/settings.toml` — default toolchain + config

**Self-update defers to the installer:** when cvm was PM-installed (its exe lives
under the brew/system prefix, not `~/.cajeta/bin`), `cvm self update` is disabled
and points at `brew upgrade cvm` / `apt upgrade cvm` / …; only the shim install
self-updates. Version-independence makes a stale PM-managed cvm harmless anyway.
Verbs: `cvm toolchain install <ver>`, `cvm default <ver>`, `cvm which`,
`cvm doctor`, per-project `cajeta-version` pin *(Phase 2)*.

### Tier 2 — IDE plugin
- **JetBrains Marketplace** (auto-publish, §8) — primary.
- **Embedded in the binary** → `cajeta ide install` — works in any env with IDEA.
- **Standalone `cajeta-idea-<ver>.zip`** — "Install plugin from Disk".

### Default-path summary (resolves D5)

| OS | Direct cajeta installer (Tier-0) | cvm-managed (Tier-1) |
|---|---|---|
| **Linux** | `.deb`/`.rpm`: payload `/usr/lib/cajeta/`, symlink `/usr/bin/cajeta`; man `/usr/share/man/man1/cajeta.1`; docs `/usr/share/doc/cajeta/` | `~/.cajeta/bin/cajeta` (+ `~/.cajeta/versions/<ver>/`) |
| **macOS** | `.pkg`: payload `/usr/local/lib/cajeta/`, symlink `/usr/local/bin/cajeta` | `~/.cajeta/bin/cajeta` |
| **Windows** | `.msi`: `C:\Program Files\Cajeta\bin\cajeta.exe` (+ system PATH) | `%USERPROFILE%\.cajeta\bin\cajeta.exe` |

> cvm itself, when PM-installed, lands in the package manager's own prefix
> (`$(brew --prefix)/bin/cvm`, `/usr/bin/cvm`, …); shim-installed cvm lands in
> `~/.cajeta/bin/cvm`. cvm-managed cajeta always lives under `~/.cajeta`.

> The "single place + symlink" shape (`/usr/lib/cajeta/` + `/usr/bin/cajeta`)
> matches the user's preference and future-proofs for additional shipped files
> (build tool, stdlib `.cja`); today the payload dir holds essentially one file.

---

## 3. Foundation: `install()` rules + CPack (all platforms)

- [ ] **Make the binary self-contained (D2):** force static linkage of the heavy
      deps; verify `ldd`/`otool -L` shows only the base runtime. *(**Windows arm
      DONE + verified** — DLLs bundled via `RUNTIME_DEPENDENCY_SET`, runs under a
      clean PATH. **Linux/macOS static-link pending a Linux runner** to validate
      `ldd`.)*
- [x] `include(GNUInstallDirs)`; `install(TARGETS cajeta ...)` to the §2 payload
      dir + a **symlink into the PATH dir** (D5: `/usr/lib/cajeta/` →
      `/usr/bin/cajeta`). *(Top-level `CMakeLists.txt`: Unix → `lib/cajeta/cajeta`
      payload + relative `bin/cajeta` symlink via `install(CODE)`; Windows →
      `bin/cajeta.exe` directly. Symlink branch validated on Linux/macOS only.)*
- [x] Install `LICENSE`, `README.md`, `VERSION`, man page (`cajeta.1`).
      *(Added MIT `LICENSE` at repo root; `man/cajeta.1`; man page Unix-only.)*
- [x] Verify `cmake --install build --prefix /tmp/x` lays out the tree and
      `bin/cajeta --version` runs. *(Verified on Windows: `bin/cajeta.exe` +
      `share/doc/cajeta/{README.md,LICENSE,VERSION}`; installed binary prints
      `cajeta 0.5.1`.)*
- [x] `include(CPack)` with core metadata from `VERSION` + D6.
- [x] Per-generator config isolated in `cmake/CPackOptions.cmake`
      (`CPACK_PROJECT_CONFIG_FILE`) — DEB/RPM metadata + WiX/productbuild scaffolds.
- [x] `cpack -G TGZ` reproduces today's tarball shape (proves install rules
      complete before native formats). *(Verified via `cpack -G ZIP` on Windows:
      `cajeta-0.5.1-AMD64.zip` with `bin/` + `share/doc/cajeta/` + SHA256.)*

---

## 4. Linux packages — deb / rpm (+ Arch via AUR)

- [~] **DEB** (`cpack -G DEB`): config done — `SHLIBDEPS ON` (base-system deps),
      `DEB-DEFAULT` name, `devel` section, **`postinst` prints the `cajeta ide
      install` hint** (`packaging/linux/deb-postinst`). Payload layout + the
      `dpkg-deb --info`/`lintian` checks need a Linux runner (and the D2
      static-link for clean deps).
- [~] **RPM** (`cpack -G RPM`): config done — `AUTOREQ`/`AUTOPROV`, MIT license,
      `RPM-DEFAULT` name, **`%post` hint** (`packaging/linux/rpm-post.sh`).
      `rpm -qpi`/`rpmlint` need a Linux runner.
      > **Channel-model note (resolved):** AUR and Homebrew now ship **`cvm`**,
      > not `cajeta` (§2 / D12) — the package ecosystems carry the evergreen
      > version manager; `cajeta` itself is a Tier-0 direct download (the `.deb`/
      > `.rpm` above, the `.pkg`, the `.msi`, the tarball). So the two bullets
      > below moved from cajeta to cvm.
- [~] **Arch AUR `PKGBUILD` for `cvm`** (per D1/D12; not CI-gated):
      `packaging/arch/PKGBUILD` installs the prebuilt `cvm` binary to
      `/usr/bin/cvm`; release automation fills `pkgver` + `sha256sums`. *(Template
      converted to cvm; finalizes when cvm is built — D12 Phase 1.)*
- [~] **Homebrew tap for `cvm`** (per §2/D12): `packaging/homebrew/cvm.rb` —
      installs the prebuilt `cvm` binary (not build-from-source: cvm is
      Cajeta-compiled, chicken-and-egg) + a caveat to run `cvm` to finish
      `~/.cajeta` setup. Lives in `jklappenbach/homebrew-tap`. *(Template ready;
      finalizes when cvm is built — D12 Phase 1.)*
- [ ] **Install-test in clean containers:** `ubuntu:24.04` (`apt install
      ./*.deb`), `fedora:latest` (`dnf install ./*.rpm`), best-effort
      `archlinux:latest` (makepkg) — each `--version` + compile smoke, **no extra
      repos**. *(Needs a Linux runner.)*

---

## 5. Windows — MSI

- [x] Install the **WiX Toolset** in the build env. **Pinned to WiX v5**
      (`dotnet tool install --global wix --version 5.0.2` + `wix extension add
      -g WixToolset.UI.wixext/5.0.2`). **v6/v7 require accepting the Open Source
      Maintenance Fee (OSMF) EULA** (`WIX7015`) — a licensing gate we avoid by
      staying on v5, the last pre-OSMF major. CMake's CPackWIX uses the modern
      `wix` CLI when `CPACK_WIX_VERSION >= 4` (set to 4 in `CPackOptions.cmake`).
- [x] `cpack -G WIX`: stable `CPACK_WIX_UPGRADE_GUID`, product/manufacturer,
      program-menu folder, **PATH** entry, install to `C:\Program Files\Cajeta\`.
      *(Verified: MSI builds; admin-extract shows `Program Files\Cajeta\bin` +
      `share\doc\cajeta`; `Environment` table appends `bin` to system PATH and
      removes on uninstall. License `.txt` staged for WiX's extension check;
      `CMP0207` policy opt-in added for `GET_RUNTIME_DEPENDENCIES`.)*
- [x] **Bundle MinGW runtime DLLs** beside `cajeta.exe` — resolved via CMake
      `install(... RUNTIME_DEPENDENCY_SET)` (transitive `file(GET_RUNTIME_
      DEPENDENCIES)`, not a hardcoded list). *(Verified: 13 DLLs bundled incl.
      `libLLVM-22.dll`; installed binary runs under a clean PATH with no MinGW.)*
- [ ] **Optional plugin feature** (D8): surface the plugin post-install. **Design
      note:** a per-machine MSI custom action runs as **LocalSystem**, so it must
      NOT auto-run `cajeta ide install` (that would target the SYSTEM profile, not
      the user's). Plan: a final-dialog/readme hint telling the user to run
      `cajeta ide install` (now on PATH). The embed + verb (§7) are done; only the
      MSI hint UI remains.
- [ ] Build + **install-test on a clean Windows runner**: `msiexec /i … /qn`,
      fresh shell, `--version`, compile a sample, plugin-feature on/off,
      uninstall cleanly. *(Partial: admin-extract (`msiexec /a`) payload verified
      locally; full elevated silent install + uninstall is a clean-VM/CI step.)*

---

## 6. macOS — `.pkg`

- [~] `cpack -G productbuild`: config done — identifier `dev.cajeta.compiler`,
      version from `VERSION`, `CPACK_PACKAGING_INSTALL_PREFIX=/usr/local`
      (→ `/usr/local/lib/cajeta/cajeta` + `/usr/local/bin/cajeta` symlink).
      welcome/license/conclusion resources still to add on a Mac runner.
- [x] **Optional plugin component** (D8): `packaging/macos/postinstall` runs
      `cajeta ide install` **as the console user** (`stat -f%Su /dev/console` +
      `sudo -u`), best-effort (always exits 0). Wired via
      `CPACK_POSTFLIGHT_CAJETA_SCRIPT`.
- [ ] **Sign + notarize** (D3): `productsign` with Developer ID Installer →
      `notarytool` submit → `stapler staple`. Secrets: signing identity + App
      Store Connect API key. *(Needs a Mac + Developer ID.)*
- [ ] Build + **install-test on a clean macOS runner**: `installer -pkg … -target
      /`, fresh shell, `--version`, compile a sample, plugin component on/off.
      *(Needs a Mac.)*

---

## 7. IDE plugin — build + embed

- [x] Wire `gradlew buildPlugin` into the release build → the plugin zip
      (`ide-plugins/idea/build/distributions/cajeta-idea-<version>.zip`). *(New
      `plugin` job in `release.yml`, built once; `build` matrix `needs: plugin`
      and downloads the zip before configuring.)*
- [x] Provision a full **JDK 21** in CI for the plugin build. *(plugin job
      `actions/setup-java@v4` temurin 21.)*
- [x] **Sync `pluginVersion` to repo `VERSION`** at build time (D9).
      *(`-PpluginVersion=$(cat VERSION)`; the compiler also bakes
      `CAJETA_PLUGIN_VERSION="${CAJETA_VERSION}"` — `cajeta ide list` shows it.)*
- [~] `gradlew verifyPlugin` against `242 … 261.*`. *(Wired in the plugin job as
      `continue-on-error` — the JetBrains verifier downloads IDE images and is
      flaky/slow; tighten into a hard gate once proven stable in CI.)*
- [x] **Embed the plugin zip into the `cajeta` binary** (D8) — mirrors the stdlib
      embed (`xxd -i` → generated `cajeta_plugin_embedded.cpp`; stub when no zip).
      Plugin build runs **before** the compiler build via `needs: plugin`.
      *(Verified locally: 18 MB zip embedded, links clean.)*
- [x] Implement **`cajeta ide install`** (D8): `src/cajeta/cli/IdeCommands.{h,cpp}`
      dispatched from `main.cpp` beside `archive`/`jit-run`/`dap`. Detects IDEA
      config dirs per-OS (Win `%APPDATA%\JetBrains\<p>\plugins`, mac
      `…/Application Support/JetBrains/<p>/plugins`, Linux
      `~/.local/share/JetBrains/<p>`), extracts the embedded zip via a compact
      zlib raw-inflate ZIP reader; idempotent; `uninstall` + `list` counterparts;
      `--plugins-dir=` override. *(Verified: detects both real IDEA installs;
      extraction is **CRC-perfect** vs the source zip across all 12 jars.)*
- [x] **Surface subcommands in `--help`**: added a "Subcommands" section to
      `printUsage` listing `archive`/`ide`/`jit-run`/`dap`. *(Verified.)*
- [x] Publish `cajeta-idea-<ver>.zip` as a **standalone release artifact** too
      (Install-from-Disk users). *(plugin job uploads it; release glob attaches
      `artifacts/**/cajeta-idea-*.zip`.)*

---

## 8. JetBrains Marketplace release

*In scope per D10; auto-published on production tags.*

- [ ] JetBrains **vendor account** + reserve `dev.cajeta.idea` on Marketplace.
      *(External — requires a JetBrains account.)*
- [x] Enrich `plugin.xml`: real `<description>`, `<change-notes>`, `<vendor>` with
      **email + url**. *(Done: `<vendor email="jklappenbach@gmail.com"
      url="…/cajeta">Julian Klappenbach</vendor>`, feature-list description,
      change-notes pointing at GitHub releases. XML validated.)*
- [x] **Signing**; wire `signPlugin` (`CERTIFICATE_CHAIN`, `PRIVATE_KEY`,
      `PRIVATE_KEY_PASSWORD`). *(Done: `signing{}` block in build.gradle.kts +
      gated workflow step. Secrets themselves are external — step no-ops without
      them.)*
- [x] Wire `publishPlugin` with `PUBLISH_TOKEN` + channel. *(Done: `publishing{}`
      block; workflow step runs `signPlugin publishPlugin` on production tags
      only, skips pre-releases, no-ops without secrets. Stable channel; EAP
      routing deferred since pre-releases are skipped.)*
- [ ] **One-time bootstrap:** first upload by hand (JetBrains review), then
      automated. *(External — must precede the first automated publish.)*
- [ ] Verify install from inside IntelliJ (Plugins → Marketplace → "Cajeta").
      *(External — post-publish.)*
- [ ] Maintain the `untilBuild` ceiling as new IDE majors ship. *(Ongoing;
      `pluginUntilBuild=261.*` in gradle.properties.)*

---

## 9. Architecture matrix

| Arch | deb | rpm | AUR | brew | msi | mac .pkg |
|---|---|---|---|---|---|---|
| x86_64 / amd64 | ✓ | ✓ | ✓ | ✓ (Linuxbrew) | ✓ | — |
| aarch64 / arm64 | ✓ | ✓ | ✓ | ✓ | (D7) | ✓ |
| riscv64 | D4 (deferred) | D4 | — | — | — | — |

*(Intel macOS = build-from-source. IDE plugin zip is arch-independent.)*

- [ ] x86_64 builds natively on `ubuntu-latest` / `windows-latest`.
- [ ] aarch64 on `ubuntu-24.04-arm` (deb/rpm via containers/QEMU) + `macos-14`
      (`.pkg`).
- [ ] Verify embedded **arch labels** (`amd64`/`arm64` deb; `x86_64`/`aarch64` rpm).

---

## 10. Release workflow integration

- [x] **Packaging stage** in `release.yml` after `build`, emitting deb/rpm/msi/
      `.pkg` per `(format × arch)`. *(Added "Build native installers" step:
      Linux → `cpack -G DEB` + `-G RPM`, Windows → `-G WIX`, macOS →
      `-G productbuild`. `continue-on-error: true` for now — must not red the
      green tarball release until §4/§6 validate each into a gate.)*
- [x] **IDE-plugin build job** (JDK 21 + Gradle) producing the zip once; fan into
      the compiler build for embedding (ordering: plugin → embed → link).
      *(Done in §7: `plugin` job + `build needs: plugin` + download-before-
      configure.)*
- [x] Provision tooling: WiX (Windows — `dotnet tool install wix@5.0.2` + UI ext,
      `continue-on-error`), `productbuild` (macOS, preinstalled), `dpkg-deb`
      (Ubuntu, preinstalled) + `rpmbuild` (`rpm` added to the Linux apt list).
      *(macOS notarization is §6.)*
- [x] Consistent names: `cajeta_<ver>_<arch>.deb` / `cajeta-<ver>-1.<arch>.rpm`
      (`DEB-DEFAULT`/`RPM-DEFAULT`), `.msi` / `.pkg` via `CPACK_PACKAGE_FILE_NAME`.
      *(`Cajeta-<ver>.zip` plugin name is §7.)*
- [x] Extend `upload-artifact` + `softprops/action-gh-release` globs to attach
      installers (`build/*.{deb,rpm,msi,pkg,sha256}` → `artifacts/**/*.{…}`).
      *(plugin zip glob pending §7.)*
- [ ] Guarded **`publishPlugin`** step (D10) — production tags only, never dry-run.
      *(§8 — pending.)*
- [ ] (D12) **Build `cvm` with the just-built compiler** (`cajeta
      --emit=exe tools/cvm/…`) on each target and ship its binary as a
      release artifact; host/refresh the `sh.cajeta.dev` bootstrap shim; update
      the Homebrew tap / winget manifest / AUR PKGBUILD each release.
- [x] Preserve **dry-run**: builds all installers, publishes nothing. *(Packaging
      runs in the `build` job regardless of mode; only the `release` job's publish
      is gated on `mode != dry-run`, so dry-run uploads installers as artifacts.)*

---

## 11. Test the GitHub release process end-to-end

- [ ] **Dry-run** produces every installer + plugin zip — no tag, no Release, no
      Marketplace push.
- [ ] **Install-test matrix:** each Linux package in its clean container; MSI
      silent-install on clean Windows; `.pkg` via `installer` on clean macOS —
      each `--version` + compile smoke + plugin toggle.
- [ ] **Plugin install-test:** `cajeta ide install` lands the plugin in a clean
      IntelliJ and it loads (`verifyPlugin` + manual sanity launch).
- [ ] **Pre-release tag** (`v<next>-rc1`): Release page shows full set,
      pre-release-flagged; download + install one of each on clean envs.
- [ ] **Production tag**: "latest" carries all installers + plugin; Marketplace
      updated via `publishPlugin`.
- [ ] Partial-failure posture holds (`fail-fast: false`).

---

## 12. Documentation

- [x] Update `RELEASING.md`: channel matrix (§2), installer/plugin artifact
      shapes, signing/notarization tooling, install-test matrix, Marketplace flow.
      *(Done: expanded "Artifact shape" + new "Distribution channels", "IDE plugin
      & JetBrains Marketplace", "Installer tooling provisioned in CI", and
      "Install-test matrix" sections.)*
- [x] `README.md` install section: per-channel one-liners — direct-download +
      local install for `.deb`/`.rpm`/`.msi`/`.pkg`/tarball, planned
      `apt`/`dnf`/AUR/`brew`/`winget`/`sh.cajeta.dev` one-liners, and `cajeta ide
      install`. *(Done: new "Installing" section + ToC entry.)*

---

## 13. Definition of Done

- [ ] `cmake --install` + `cpack` produce packages locally on each host.
- [ ] `.deb` / `.rpm` exist for **x86_64 and aarch64**, install cleanly in clean
      containers with no extra repos.
- [ ] An **AUR `PKGBUILD`** and a **Homebrew tap** install cleanly (best-effort,
      not release gates).
- [ ] A Windows **`.msi`** installs on a clean machine, puts `cajeta` on PATH,
      `--version` + compile work, uninstalls cleanly.
- [ ] A macOS **`.pkg`** (signed + notarized for "latest") installs on a clean
      Mac, `--version` + compile work.
- [ ] The release build **builds + embeds the IDE plugin**; **`cajeta ide
      install`** installs it into IntelliJ on any OS; `cajeta --help` lists the
      `ide` subcommand.
- [ ] `release.yml` builds **all** installers + plugin zip automatically and
      attaches them (production Release / dry-run artifacts).
- [ ] The IDEA plugin **auto-publishes to the Marketplace** on production tags
      (after the one-time manual first upload).
- [ ] **`cvm` (written in Cajeta) compiles to a binary** with the released
      compiler on each target; the `sh.cajeta.dev` bootstrap fetches it into
      `~/.cajeta/bin` and wires PATH.
- [ ] A dry-run → pre-release → production cycle has been exercised end-to-end.
- [ ] `RELEASING.md` + `README.md` document the channels, paths, plugin, tests.
- [ ] All §1 decisions resolved (D5/D6/D7/D12 closed out).
