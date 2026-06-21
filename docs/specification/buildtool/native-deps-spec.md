# Native Dependency Subsystem — Specification

> Status: **draft** (design). How Cajeta libraries declare, distribute, resolve,
> and link native C/C++ libraries — with **zero developer declaration** and
> **offline / airgapped / JIT safety**. A build-tool + Olla + `.cja`-format +
> `@Native`-resolver capability. First consumer: the codec library's compression
> layer (zstd/zlib/brotli). Open calls awaiting developer confirmation are marked
> **⟨CONFIRM⟩**.

---

## 1. Definition

### 1.1 Purpose
Some Cajeta libraries must call into native C/C++ code (e.g. heavy compression
codecs that are impractical to reimplement). This subsystem makes that **invisible
to the application developer** and **safe to run with no network** — including
JIT execution on an airgapped host.

### 1.2 Problem
A native library is a per-platform binary with linker inputs (static archives,
shared objects, import libs, headers). Naively this forces every downstream
developer to find, install, version-match, and declare it — and breaks the moment
the execution host has no internet. We want native use to be a property a library
carries, resolved automatically, with the artifacts available offline.

### 1.3 Scope
- Declaring a native requirement on a Cajeta binding and in a library manifest.
- Packaging native artifacts inside a `.cja`.
- Transitive, automatic resolution across a dependency graph.
- Linking for both AOT (`--emit=exe`) and JIT.
- Offline / airgapped provisioning and fail-loud diagnostics.
- Olla's role as a per-platform native-binary mirror.

### 1.4 Non-goals
- Building native libraries from source on the user's machine (we consume
  prebuilt artifacts; a library *publisher* may build at publish time, out of
  scope here).
- A general FFI/marshalling design beyond the existing `@Native` symbol binding.
- Resolving **whether** a given format should bind native vs be reimplemented
  (that is each library's design decision — see §10.1).

### 1.5 Terms
- **`.cja`** — Cajeta archive (bitcode + source + metadata), jar-like.
- **`@Native`** — annotation binding a Cajeta method to a native symbol (exists
  today for runtime symbols; §2 extends it to external libraries).
- **native artifact** — a per-platform linker input: `.a` / `.so` / `.dylib` /
  `.lib` / `.dll`, plus headers under `include/`.
- **redistributable** — the artifact's license permits us to ship it inside a
  `.cja` / mirror it on Olla.
- **provision time** — `.cja` publish, consumer install, first resolve, or an
  explicit `cajeta fetch` / `vendor`. **Run time** — AOT execution or JIT.
- **resolver** — the build-tool component that satisfies native requirements.
- **ubercja** — a fat archive bundling a library or app together with its Cajeta
  dependencies *and* native artifacts into one self-contained `.cja` (the
  uber-jar analogue).
- **packaging step** — a **default** stage of `.cja` / ubercja build that bakes
  the resolved native artifacts into the archive's `native/` tree (§3.3). It runs
  wherever the artifacts are legally available to the packager — at the publisher
  for redistributable libs, and at the **downstream app build** for embargoed
  libs the developer has provisioned.
- **embargoed** — a native lib we (Cajeta/Olla) may not redistribute publicly, but
  which the developer building an app *has the right to obtain and to ship inside
  their own archive*.

### 1.6 Core invariants (requirements that hold everywhere)
- **INV-1 Zero developer declaration.** An application developer never declares a
  transitive native dependency. The only knob is an optional version override
  (§4.3).
- **INV-2 Network only at provision time.** Resolution may touch the network at
  provision time; **execution and JIT never do.** Missing-and-offline →
  fail-loud, never a silent runtime fetch or hang (§9).
- **INV-3 Self-contained by default.** A `.cja` whose native deps are all
  redistributable carries everything needed to link offline on any supported
  platform (§3).
- **INV-4 Fail loud, never silent.** An unsatisfiable native requirement raises a
  precise, actionable error (which lib, version, platform, where to put it) — it
  is never silently skipped or mis-linked (mirrors the codec fail-loud rule).

---

## 2. Declaration — how a library states its native need

### 2.1 Requirements
- A binding references a native **symbol** and the **library id** that provides
  it. **⟨CONFIRM 2.A⟩** Recommended shape:
  `@Native(symbol = "ZSTD_compress", lib = "zstd")` — symbol + lib-id only; the
  heavy metadata is *not* repeated per binding.
- The library's own `cajeta.json` carries a **`native-libraries`** block
  **⟨CONFIRM 2.B: section name⟩**, keyed by lib-id, holding: default version
  constraint, supported platforms, link mode (§5), artifact source/coordinates,
  `sha256`, SPDX license, a `redistributable` boolean, and (for
  non-redistributable) human acquisition instructions.
- The `.cja` build embeds this block into the archive metadata so it travels with
  the library (enables transitive resolution, §4).

### 2.2 Use cases
- **2.2.1** As a library author, when I bind a C function, then I annotate the
  method `@Native(symbol=…, lib=…)` and the compiler emits an external-symbol
  reference resolved at link time.
- **2.2.2** As a library author, when I declare `zstd` in my `native-libraries`
  block with a default version + per-platform coordinates + `redistributable:
  true`, then consumers resolve it automatically with no action.
- **2.2.3** As a library author shipping a binding to a non-redistributable lib,
  when I set `redistributable: false` and provide acquisition instructions, then
  the toolchain refuses to bundle it and routes consumers to the provisioning
  path (§6).

---

## 3. Packaging — the `.cja` `native/` tree

### 3.1 Requirements
- A `.cja` gains a **`native/`** tree: `native/<os>-<arch>/` holding the linker
  artifacts per platform, plus a shared **`include/`** for headers/glue.
  **⟨CONFIRM 3.A: platform-triple naming⟩** (e.g. `linux-x64`, `linux-arm64`,
  `macos-arm64`, `windows-x64`).
- The **publishing** library's build fetches/builds the **redistributable**
  artifacts per target platform and bundles them into its `.cja/native/`. A
  consumer links straight from there — **downloads nothing**.
- **⟨CONFIRM 3.B: bundle breadth default⟩** — bundle-all-supported-platforms
  (max portability, larger `.cja`) vs bundle-host-only vs slim-`.cja`+fetch.
  Recommended default: **bundle all platforms the publisher targets**; allow a
  slim opt-out.
- **Embargoed** artifacts are never placed in a *publicly distributed* `.cja` and
  never mirrored on Olla — but they **are** baked into the **downstream app's**
  `.cja` / ubercja by the packaging step (§3.3), within the app developer's
  redistribution rights.

### 3.2 Use cases
- **3.2.1** As a publisher, when I build `cajeta-codec` with a redistributable
  `zstd` for four platforms, then the resulting `.cja` contains
  `native/{linux-x64,linux-arm64,macos-arm64,windows-x64}/libzstd.a` +
  `include/zstd.h`.
- **3.2.2** As a consumer on `linux-arm64`, when I depend on that `.cja`, then the
  linker selects `native/linux-arm64/` with no network and no declaration.
- **3.2.3** As a publisher, when I opt into a slim `.cja`, then `native/` is
  omitted and consumers resolve via fetch/cache (§4, §6) at provision time.

### 3.3 The packaging step (default, self-contained)
**Requirements**
- Building a `.cja` or **ubercja** runs a **default packaging step** that bakes
  every resolved native artifact that is **locally available to the packager**
  into the archive's `native/<platform>/` tree — so the archive is self-contained
  by default. Opt out explicitly for a slim archive.
- The step runs wherever the artifacts are legally in hand: the **publisher** for
  redistributable libs (→ a self-contained published `.cja`, also Olla-mirrorable),
  and the **downstream app build** for **embargoed** libs the developer has
  provisioned (→ a self-contained app `.cja`/ubercja the developer may ship within
  their rights). The `redistributable` flag gates only **public mirroring**
  (Olla) and **publisher** bundling of a *publicly* distributed library — it does
  not stop the app developer baking a provisioned embargoed lib into *their* app
  archive.
- The packaging step is the **primary** way embargoed libs reach airgapped-JIT
  hosts: bake once at app-package time, ship the archive (§6).

**Use cases**
- **3.3.1** As an app developer using an embargoed native lib I have provisioned,
  when I package my app, then the default packaging step bakes the embargoed
  artifact into my app `.cja`/ubercja `native/` tree — no extra flags.
- **3.3.2** As an app developer, when I build an **ubercja**, then it bundles my
  Cajeta deps *and* all resolved native artifacts (redistributable + provisioned
  embargoed) into one self-contained archive.
- **3.3.3** As an app developer, when I want consumers to resolve natives
  externally, then I opt the packaging step out (slim archive) and accept the
  provision-time resolution path (§4).

---

## 4. Resolution — transitive, automatic, zero declaration

### 4.1 Requirements
- The resolver collects the **union of native requirements** across all `.cja`
  dependencies (transitively) from their embedded `native-libraries` metadata.
- **Probe order** to satisfy each requirement (first hit wins):
  1. the dependency `.cja`'s own `native/<platform>/`
  2. the project-local vendored `native/` dir
  3. the user cache `~/.cajeta/native/<lib>/<ver>/<platform>/`
  4. a system-provided library (dynamic mode only, §5)
  5. **provision-time** fetch (Olla mirror / declared source) if redistributable
     or license already accepted
  6. else → **fail loud** (§9)
- **⟨CONFIRM 4.A: version-conflict policy⟩** when two deps require different
  versions of the same lib. Recommended: highest-compatible (semver) wins; an
  explicit developer override always wins; incompatible majors → fail-loud.

### 4.2 Use cases
- **4.2.1** As an app developer, when I add a `.cja` that transitively needs
  `zstd`, then it links with **no native declaration anywhere in my project**.
- **4.2.3** As an app developer, when two libs need `zstd` 1.5.2 and 1.5.6, then
  the resolver links one compatible 1.5.x and reports the choice.

### 4.3 Version override (the one developer knob)
- **4.3.1** As an app developer, when I want a specific/newer native version, then
  I add an override in my `cajeta.json` (`native-overrides: { zstd: "1.5.6" }` or
  a local path) — the **only** native entry I ever write — and it wins over
  library defaults.

---

## 5. Linking — AOT vs JIT, static vs dynamic

### 5.1 Requirements
- **⟨CONFIRM 5.A: default link mode⟩** Recommended: **static by default**
  (self-contained, offline-portable binaries); **dynamic** is opt-in for
  system-provided libs.
- **AOT `--emit=exe`** resolves at build time and statically links → the exe is
  self-contained and needs nothing at run time.
- **JIT** links/`dlopen`s the artifact from the `.cja`'s `native/`, the vendored
  dir, or the cache — chosen at provision time, **loaded with no network**.

### 5.2 Use cases
- **5.2.1** As a user, when I run an AOT exe built with a bundled `zstd` on an
  offline host, then it runs — the lib is statically inside the exe.
- **5.2.2** As a user, when I JIT-run on an airgapped host and the `.cja` bundles
  the platform artifact, then the JIT links it from `native/` with no network.
- **5.2.3** As a user opting into dynamic system linking, when the system `libz`
  is present, then the exe links it and we ship nothing.

---

## 6. Offline / airgapped / portability

### 6.1 Requirements
- **Redistributable** libs → bundled in `.cja/native/` → fully offline, zero
  consumer download (INV-3).
- **Embargoed** libs → **primary path: the default packaging step (§3.3) bakes the
  developer-provisioned artifact into the app `.cja`/ubercja**, so the shipped
  archive is self-contained for airgapped JIT. Fallbacks (when not packaging into
  the archive): the project `native/` vendor dir, the `~/.cajeta/native/…` cache,
  or a manifest/env-declared path — all no-network at run time.
- `cajeta fetch` / `cajeta vendor` pre-populate the cache / vendor dir while
  online; `cvm` may pre-seed common libs at toolchain install.

### 6.2 Use cases
- **6.2.1** As a user on an airgapped host, when I JIT-run a `.cja` whose native
  dep is redistributable-and-bundled, then it works with no network.
- **6.2.2** As a developer targeting airgapped JIT with an embargoed lib, when I
  package my app, then the default packaging step (§3.3) bakes the
  provisioned artifact into the app `.cja`/ubercja, and the airgapped host
  JIT-links it straight from the archive with no network. (Vendoring into
  `native/`/cache is the fallback when not packaging into the archive.)
- **6.2.3** As a developer, when I run `cajeta vendor` before going offline, then
  every required native artifact is materialized locally.
- **6.2.4** As a user, when a required artifact is absent and the host is offline,
  then resolution **fails loud** with the exact lib/version/platform and where to
  place it (§9) — never a runtime net call.

---

## 7. License & redistribution

### 7.1 Requirements
- Each `native-libraries` entry carries an SPDX license id + a `redistributable`
  boolean. The flag governs **public** distribution only: redistributable →
  eligible for **publisher** bundling into a publicly distributed `.cja` and for
  **Olla mirroring**.
- **Embargoed** (not redistributable) → never publisher-bundled or Olla-mirrored;
  Cajeta never auto-fetches it. The **app developer** obtains it (license
  acceptance recorded once if a source permits guided fetch) and the packaging
  step (§3.3) may bake the provisioned copy into the developer's **own** app
  archive, within the developer's redistribution rights.

### 7.2 Use cases
- **7.2.1** As a publisher with a BSD/zlib/Apache native lib, when I mark it
  redistributable, then it bundles + mirrors automatically.
- **7.2.2** As a user needing a non-redistributable lib, when I have not provided
  it, then I get acquisition instructions and (if the source permits) a
  license-acceptance prompt before any fetch.

---

## 8. Olla registry role

### 8.1 Requirements
- Olla mirrors **redistributable** native binaries per platform (id × version ×
  platform), checksum-addressed, for the provision-time fetch path (probe step 5).
- For non-redistributable libs Olla serves **metadata only** (coordinates,
  checksum, acquisition instructions) — not the binary.

### 8.2 Use cases
- **8.2.1** As the resolver, when a redistributable artifact is not yet local,
  then I fetch it from the Olla mirror and verify its checksum, then cache it.
- **8.2.2** As the resolver, for a non-redistributable lib, then Olla gives me the
  metadata to locate/verify a user-provided artifact, never the binary.

---

## 9. Failure & diagnostics (fail-loud)

### 9.1 Requirements
- A missing/unsatisfiable requirement produces a precise message: lib id, resolved
  version, target platform, the locations probed, and the concrete fix (install
  command, download URL + sha256, vendor path, or env var).
- Checksum mismatch and unsupported-platform are distinct, named failures.

### 9.2 Use cases
- **9.2.1** Missing + offline → "zstd 1.5.x for linux-arm64 not found (probed:
  .cja/native, ./native, ~/.cajeta/native; offline). Run `cajeta fetch` online,
  or place libzstd.a at ./native/linux-arm64/."
- **9.2.2** Checksum mismatch → named failure naming expected vs actual sha256.
- **9.2.3** Unsupported platform → names the platform and the platforms available.

---

## 10. Open / out of scope

- **10.1 Codec breadth (separate decision).** Whether "libraries downloaded"
  reopens native binding for the **format readers** (Parquet/ORC/Avro) or stays
  **compression-only** is a *codec-plan* decision, not settled here. This
  subsystem is agnostic to it.
- **10.2** Confirm items **2.A/2.B, 3.A/3.B, 4.A, 5.A** above.
- **10.3** Building native artifacts from source (publisher side) — future.
