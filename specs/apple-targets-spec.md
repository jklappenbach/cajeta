# Cajeta Apple Targets (iOS / iPadOS / tvOS) — Spec

> Status: **DRAFT for review** (design skill). Specs shipping cajeta code in
> iOS, iPadOS, and tvOS applications: cross-compilation to Apple device and
> simulator triples, the runtime port, a static-library artifact consumable
> from a Swift/Xcode shell (Tier 1), and the Objective-C FFI groundwork that
> makes full-cajeta apps (Tier 2) an extension rather than a rework.
>
> Grounds in [`docs/BuildTool.md`](../docs/specification/buildtool/BuildTool.md)
> (`${target}` triple, `cajeta toolchain`),
> [`native-deps-spec.md`](../docs/specification/buildtool/native-deps-spec.md)
> (`@Native`, per-platform coordinates), and the portability survey of
> `src/cajeta/compile/Compiler.cpp` (target machine, link path) and
> `runtime/native/` (fibers, reactor, platform gates). Plan at
> `agents/apple-targets-plan.md` once approved.

---

## 1. Definition

### 1.1 Purpose
Make Apple mobile platforms (iPhone, iPad, Apple TV) shippable targets for
cajeta. A developer writes application logic in cajeta, builds it into a
linkable artifact for `arm64-apple-ios` / `arm64-apple-tvos` (device and
simulator), links it into a thin Swift/Xcode shell that owns the UI, and
submits the result to the App Store.

### 1.2 Tiers
- **Tier 1 (this spec's deliverable):** cajeta as the application core — a
  static-library artifact with a C-ABI surface, consumed by a Swift shell.
- **Tier 2 (follow-up spec):** full-cajeta apps — a generated Objective-C
  binding library (`dev.cajeta.apple.*`) so `main()` in cajeta drives
  `UIApplicationMain` and UIKit directly. This spec builds Tier 2's FFI
  substrate (§6) but not the bindings.
- **Tier 3 (roadmap, out of scope):** cajeta-native UI rendering via a Metal
  xpu backend.

### 1.3 Scope (v1)
- **Target triples** `arm64-apple-ios`, `arm64-apple-ios-simulator`,
  `arm64-apple-tvos`, `arm64-apple-tvos-simulator`, cross-compiled from a
  macOS host against Apple SDKs (§2).
- **Runtime port**: replace the `ucontext` fiber engine (removed from the iOS
  arm64 SDK), gate JIT/`dlopen`/GPU paths off on embedded Apple platforms
  (§3).
- **Static-library emit** (`--emit=staticlib`) with a generated C header and
  XCFramework packaging (§4).
- **Swift-shell integration**: a documented quickstart + template so Xcode
  owns signing, Info.plist, and assets (§5).
- **Obj-C FFI groundwork**: framework linking for `@Native`, outbound C
  function pointers, and the foreign-handle retain/release convention (§6).
- **CI**: macOS-hosted build legs producing the Apple artifacts plus a
  simulator smoke test (§7).

### 1.4 Problem
Cajeta compiles and links for `aarch64-apple-darwin` (macOS) today, but
nothing mobile: the fiber engine needs `makecontext`/`swapcontext`, which the
iOS/tvOS SDKs do not provide on arm64; link flags and OS libraries are chosen
by the compiler host's `#ifdef`, not the requested `--target`
(`Compiler.cpp:2618`); there is no static-library emit mode, no SDK/sysroot
handling, and no packaging story an Xcode project can consume. iOS forbids
JIT, so the `cja`/uber/debug run paths cannot exist on-device and must be
cleanly compiled out rather than failing at runtime.

### 1.5 Constraints & dependencies
- **macOS build host required.** Apple's SDK licensing rules out cross-linking
  from Linux. All Apple-target builds run on a macOS host with Xcode;
  elsewhere the compiler emits a clear diagnostic (§2.2.5). CI already has a
  macos-14 leg (`release.yml`).
- **No JIT, no dynamic code loading on device.** Only the AOT `--emit`
  paths apply. App Store review also forbids `dlopen` of non-bundled dylibs.
- **No Vulkan on iOS/tvOS.** xpu falls back to the CPU backend; the Metal
  backend is Tier 3.
- **`__APPLE__` is not enough.** The platform layer must distinguish macOS
  from embedded Apple (`TARGET_OS_IPHONE`/`TARGET_OS_TV` from
  `TargetConditionals.h`); the runtime currently conflates them.
- **Memory model is an asset.** No tracing GC, no stop-the-world, no
  signal-based collection — the runtime port is subsystem work (fibers,
  gating), not a collector port.
- **Buildtool alignment.** Target selection rides the existing `${target}`
  triple and `cajeta toolchain` provisioning (`BuildTool.md`); per-platform
  native coordinates follow `native-deps-spec.md`.

### 1.6 Non-goals (v1)
- The Objective-C **binding generator** and generated `dev.cajeta.apple.*`
  libraries (Tier 2 spec).
- A cajeta UI toolkit or **Metal** xpu backend (Tier 3).
- **On-device debugging** (the DAP path is JIT-in-process today; remote AOT
  debugging is `embedded-targets-spec.md` §15.1 territory).
- **kqueue reactor.** The Darwin `select()` fallback is accepted for v1; a
  kqueue engine is a follow-up perf item.
- Mac Catalyst, watchOS, visionOS, and macOS `.app` bundle packaging.
- Obj-C **blocks** construction from cajeta (deferred to Tier 2; noted in
  §6).

---

## 2. Target triples & cross-compilation

### 2.1 Requirements
The compiler accepts the four Apple mobile triples (with optional minimum-OS
suffix, e.g. `arm64-apple-ios15.0`) via the existing `--target` flag and the
buildtool `${target}` variable. For Apple targets it:
- resolves the correct SDK sysroot via `xcrun --sdk
  iphoneos|iphonesimulator|appletvos|appletvsimulator --show-sdk-path`;
- selects link flags, linker flavor (`ld64`), and OS libraries from the
  **requested triple**, not the compiler host's `#ifdef` — refactoring the
  link path in `Compiler.cpp:2442-2660` to be target-driven;
- embeds the minimum deployment target in emitted objects (build-version
  load command) so `ld64` and App Store validation accept them;
- on a non-macOS host, fails fast with a diagnostic naming the macOS
  requirement.
Runtime and stdlib archives for the four triples are provisioned through
`cajeta toolchain`, mirroring the existing per-target store.

### 2.2 Use cases
- **2.2.1** As a developer on macOS, when I build with
  `--target=arm64-apple-ios`, then I get Mach-O arm64 objects compiled
  against the iphoneos SDK with the deployment target recorded.
- **2.2.2** As a developer, when I build for
  `arm64-apple-ios-simulator`, then the artifact loads in the iOS Simulator
  (simulator slice, not a device slice).
- **2.2.3** As a developer, when I set `${target}` in `cajeta.json` to a tvOS
  triple, then `cajeta build` produces tvOS artifacts with no other manifest
  changes.
- **2.2.4** As a developer, when the runtime archive for an Apple triple is
  missing, then I'm offered `cajeta toolchain install` and the build proceeds
  after it.
- **2.2.5** As a developer on Linux, when I request an Apple mobile triple,
  then the build fails immediately with "requires a macOS host with Xcode",
  not a link-time error spray.

---

## 3. Runtime port

### 3.1 Requirements
- **Fibers.** Replace the `ucontext` engine
  (`runtime/native/cajeta_rt_concurrent_exec.c:129-157`) with a hand-rolled
  arm64 (AAPCS64) context switch — callee-saved GPRs, SIMD registers, sp, lr
  — used on all Apple platforms (fixing the macOS deprecation as well).
  x86_64/Linux may keep `ucontext` or adopt the same mechanism; behavior
  (guard-paged `mmap` stacks, scheduler semantics) is unchanged either way.
- **Platform gating.** A single platform layer distinguishing embedded Apple
  (`TARGET_OS_IPHONE`) from macOS. On embedded Apple: ORC JIT
  (`CajetaJitHost`), GPU driver `dlopen` (`cajeta_xpu_driver.c`), and any
  self-exe introspection are compiled out; xpu resolves to the CPU backend;
  the reactor uses the existing portable `select()` engine.
- **Runtime build.** `runtime/native/` and the stdlib compile cleanly against
  the iOS/tvOS SDKs (audit `_DARWIN_C_SOURCE` handling, signal use, and any
  API absent on embedded Apple).
- **No regression** on existing targets: Linux/macOS/Windows runtime test
  suites stay green.

### 3.2 Use cases
- **3.2.1** As a developer, when my cajeta code spawns fibers and channels on
  an iPhone, then the cooperative scheduler behaves as on Linux/macOS (same
  semantics, guard-paged stacks).
- **3.2.2** As a developer, when I build for an Apple mobile triple, then no
  JIT, `dlopen`, or Vulkan symbol reaches the link line, and App Store
  validation passes.
- **3.2.3** As a developer using xpu kernels, when I target iOS, then kernels
  run on the CPU backend and the build reports GPU-off-for-target rather
  than failing.
- **3.2.4** As a developer, when cajeta networking runs on-device, then the
  `select()` reactor serves connections correctly (perf parity with kqueue is
  a non-goal).

---

## 4. Static-library artifact & C-ABI surface

### 4.1 Requirements
- **`--emit=staticlib`**: archive the module's objects plus the runtime,
  stdlib, TLS object, and `@Native` static dependencies into a single
  self-contained `.a` (no `main`, no linker invocation).
- **Export surface.** An explicit annotation (working name `@Export`) marks
  cajeta functions as the C-ABI entry points: unmangled (or predictably
  mangled) symbol names, C-representable parameter/return types enforced at
  compile time, with a diagnostic naming the offending type otherwise.
- **Header generation.** The build emits a C header for the export surface
  (functions, opaque handle typedefs, an init/shutdown pair for the runtime)
  suitable for a Swift bridging header or module map.
- **XCFramework packaging.** A build action bundles device + simulator
  slices and the header into an `.xcframework`, the standard currency for
  Xcode and SwiftPM binary targets.
- **Runtime lifecycle.** Exported init/shutdown entry points let the shell
  start the cajeta runtime (fiber scheduler, arenas) on app launch without a
  cajeta `main`.

### 4.2 Use cases
- **4.2.1** As a developer, when I build with `--emit=staticlib`, then I get
  one `.a` and one `.h` containing exactly my `@Export` surface.
- **4.2.2** As a developer, when I mark a function `@Export` with a
  non-C-representable parameter type, then compilation fails naming the type
  and the rule.
- **4.2.3** As a developer, when I run the xcframework build action, then I
  get an `.xcframework` with ios / ios-simulator (and, when configured, tvos)
  slices that Xcode links without warnings.
- **4.2.4** As a Swift developer, when I call the generated header's init
  function then an exported cajeta function from Swift, then the runtime is
  live and the call returns the correct value.

---

## 5. Swift-shell integration & packaging

### 5.1 Requirements
- **Template/quickstart.** A documented minimal Xcode project (SwiftUI shell)
  that consumes the xcframework: runtime init in the app lifecycle, calls
  into the export surface, builds for device, simulator, and tvOS. Shipped as
  a `docs/` guide plus a template the developer copies — no Xcode project
  generation by the buildtool.
- **Build integration.** A `cajeta.json` task drives staticlib emit +
  xcframework packaging, so "rebuild the cajeta core" is one command an Xcode
  run-script phase can invoke.
- **Xcode owns distribution.** Signing, provisioning, Info.plist, asset
  catalogs, archive/upload are Xcode's job; the spec's requirement is only
  that cajeta's artifact never blocks them (bitcode-free, position-independent,
  deployment target recorded, App Store validation clean).

### 5.2 Use cases
- **5.2.1** As a developer following the quickstart, when I build and run the
  template on a simulator, then the SwiftUI shell shows a value computed by
  cajeta code.
- **5.2.2** As a developer, when I edit cajeta source and rebuild in Xcode,
  then the run-script phase rebuilds the xcframework and the app links the
  new core.
- **5.2.3** As a developer, when I archive the template app and run App Store
  validation, then it passes with no cajeta-attributable errors.
- **5.2.4** As a developer, when I retarget the template to tvOS, then the
  same cajeta core links and runs on an Apple TV (simulator acceptable for
  acceptance).

---

## 6. Objective-C FFI groundwork (Tier-2 enablers)

### 6.1 Requirements
Specced now so Tier 2 extends rather than reworks:
- **Framework linking.** `@Native` dependency coordinates gain an Apple
  `framework` form (e.g. `lib="framework:UIKit"`), emitted as `-framework
  UIKit` on the link line, with per-platform coordinates per
  `native-deps-spec.md`.
- **Symbol aliasing.** Multiple `@Native` declarations may bind the same C
  symbol with different cajeta signatures (the `objc_msgSend` typed-cast
  pattern); verified with tests, since Tier 2 rests on it.
- **Outbound function pointers.** A cajeta function can be passed to C as a
  function pointer with a stable C-ABI entry (the `IMP` requirement for
  `class_addMethod`); callbacks re-enter the runtime safely from foreign
  threads.
- **Foreign-handle convention.** A documented ownership convention for
  refcounted foreign objects (Obj-C `id`s): opaque handle type, explicit
  retain/release binding points, and an autorelease-pool scope around
  callbacks originating in cajeta. Convention + minimal primitives only; the
  typed object model is Tier 2.
- **Proof.** An integration test on macOS (same libobjc ABI, no device
  needed) drives a real Obj-C round trip via `@Native`: `objc_getClass` /
  `sel_registerName` / typed `objc_msgSend`, and a cajeta-implemented
  method registered via `objc_allocateClassPair` + `class_addMethod`.

### 6.2 Use cases
- **6.2.1** As a developer, when I declare `@Native(symbol="NSLog",
  lib="framework:Foundation")` and call it, then the link line carries
  `-framework Foundation` and the call executes.
- **6.2.2** As a developer, when I declare two `@Native` methods bound to
  `objc_msgSend` with different signatures, then both compile and dispatch
  correctly.
- **6.2.3** As a developer, when I register a cajeta function as an Obj-C
  method and Obj-C code messages it, then my cajeta function runs and its
  return value crosses back correctly.
- **6.2.4** As a developer, when a foreign callback enters cajeta off the
  main fiber scheduler thread, then the runtime handles the re-entry per the
  documented convention (no crash, no arena corruption).

---

## 7. CI & testing

### 7.1 Requirements
- **Release matrix** gains Apple mobile legs on the macos-14 runner: build
  the compiler (existing `aarch64-apple-darwin` leg), then cross-build
  runtime/stdlib archives and the template xcframework for the four triples.
- **Simulator smoke test.** CI boots an iOS simulator (`xcrun simctl`),
  installs the template app, and exercises an on-simulator runtime test
  (fibers, allocation, string/collection stdlib, a socket round trip),
  asserting output.
- **Host test additions.** The fiber context-switch engine and the §6 Obj-C
  round trip run in the existing macOS test suite; staticlib/header/
  xcframework emit have buildtool tests.
- **Graceful skip.** Apple-target tests skip (not fail) on non-macOS hosts.

### 7.2 Use cases
- **7.2.1** As a maintainer, when release CI runs, then Apple mobile runtime
  archives and the template xcframework are produced and uploaded alongside
  existing artifacts.
- **7.2.2** As a maintainer, when a runtime change breaks fiber switching on
  arm64 Darwin, then the macOS leg's tests catch it before release.
- **7.2.3** As a contributor on Linux, when I run the test suite, then Apple
  tests skip cleanly.

---

## 8. Non-functional requirements

- **8.1 Size.** The staticlib path honors tree-shaking/`--link-mode=lean` so
  a minimal app core stays small; the template app reports its size in CI
  (target envelope set during planning).
- **8.2 No collateral damage.** Existing targets' codegen, link, and runtime
  behavior are unchanged; the target-driven link refactor is
  behavior-preserving for current triples.
- **8.3 Determinism.** Same source + toolchain + SDK ⇒ identical archive
  contents (existing reproducibility posture extends to Apple artifacts).
- **8.4 Documentation.** A `docs/` platform guide covers: triples and SDK
  setup, staticlib/xcframework workflow, the export surface, the
  foreign-handle convention, and platform limits (no JIT, CPU-only xpu,
  select reactor).

---

## 9. Relationship to other specs

- **`native-deps-spec.md`** — the `framework:` coordinate form (§6.1) extends
  its per-platform dependency model.
- **`embedded-targets-spec.md`** — shares the `${target}`/`cajeta toolchain`
  selection machinery (§3 there); its remote AOT debug enabler (§15.1) is the
  future path for on-device debugging, out of scope here.
- **`windows-release-ci-spec.md`** — the pattern for adding a platform leg to
  release CI (§7).
- **Tier 2 (future)** — the Obj-C binding generator spec consumes §6 as its
  substrate; the generated libraries publish as `dev.cajeta.apple.*` via
  olla.

---

## 10. Open questions

- **10.1 Export mechanism.** New `@Export` annotation vs overloading an
  existing mechanism — depends on what the annotation layer already offers;
  resolve during planning.
- **10.2 Minimum OS versions.** Proposed iOS 15 / tvOS 15 (arm64-only line,
  wide device coverage). Confirm.
- **10.3 tvOS slices in the default xcframework** or built on demand?
  Proposed: on demand via task config.
- **10.4 Binding-generator home** (Tier 2, decide before that spec): compiler
  repo tool vs standalone `dev.cajeta.apple` project. Leaning standalone
  library project, generator included, published via olla.
