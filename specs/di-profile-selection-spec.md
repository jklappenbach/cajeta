# di-profile-selection — DI profiles & test doubles (spec)

> Status: **draft, pending approval**. Authored with the **design** skill.
> The *why* and the *what*; the actionable *how* lives in
> `agents/di-profile-selection-plan.md`.

---

## 1. Definition

### 1.1 Purpose
Complete Cajeta's compile-time DI substrate (`cajeta.aot`) so that
**profile-scoped components** and **test-double substitution** are first-class,
declared, and correct. The motivating consumer is the `cajeta-logging`
Phase-3 milestone ("LoggerFactory + DI — the basics are usable"): it wants
`@Profile prod`/`dev` wiring and a `@TestComponent` capturing appender that
replaces the real sink under `--profile=test`.

### 1.2 Current state (verified against 0.9.0, commit 32942b53)
- **`@Profile` works end-to-end.** `ComponentDescriptor::profiles`, `--profile=<name>`
  → `CajetaModule::activeProfile` (default `"prod"`), and profile-filtered
  resolution in `resolveDependencyGraph()` all exist. A probe with two `@Profile`-tagged
  `Greeter` impls selected the correct one under `--profile=prod` vs `--profile=dev`.
- **`@TestComponent` inclusion/exclusion by profile works** when the double is
  injected by its own concrete type (existing gtests cover this).
- **`@TestComponent` masking of a same-interface `@Component` is BROKEN.** The
  override set is keyed on the test class's *own* canonical name
  (`CajetaModule.cpp:707-708`), then a real component is dropped only if *its own*
  canonical name is in that set (`:718-719`). Two distinct classes never share a
  canonical name, so the mask never fires: the double becomes a second provider and
  resolution dies with `CAJETA_ERROR_DI_AMBIGUOUS` (probe-confirmed).
- **No annotation-declaration files ship** for `@Profile` or `@TestComponent`. The
  compiler recognizes them by short-name only; there is no importable, documented
  type as there is for `@Component`/`@Inject`/`@Factory`.

### 1.3 Scope
In scope (all in cajeta-two):
- Fix `@TestComponent` masking to key on **shared implemented interfaces**.
- Ship annotation-declaration files in `cajeta.aot`.
- gtest coverage for the fixed masking + a `.cajeta` tour/sample demo.

Out of scope (tracked elsewhere):
- The `cajeta-logging` `LoggerFactory` milestone itself (that repo's
  `plan/logging-plan.md` Phase 3) and the 0.9.0 build migration of
  cajeta-logging / cajeta-unit.
- Any runtime DI container. DI stays compile-time.
- Profile *selection* of a provider beyond the existing profile *filter*
  (i.e. no new tie-break rules for `@Profile`).

### 1.4 Non-goals
- `@TestComponent` masking of a component injected by its **concrete type**. Masking
  is interface-scoped: swapping requires injecting through the interface.
- Reworking `@Component`/`@Factory`/`@Inject` resolution semantics.
- **`@Repository` (and other stereotypes like `@Service`/`@Controller`).** These are
  framework stereotypes with no distinct core behavior — the compiler treats
  `@Repository` as a `@Component` alias. Their declarations belong in
  **cajeta-primavera** (the policy/enterprise layer), not the `cajeta.aot`
  substrate. `@Profile`/`@TestComponent` ship here only because their behavior is
  compiled into the core.

---

## 2. `@TestComponent` masking by shared interface

### 2.1 Requirement
Under `--profile=test`, an active `@TestComponent` masks (removes from the active
set) every non-test `@Component` that implements an interface the test double also
implements. The double takes over that interface's injection sites. Outside test
mode, `@TestComponent`s are dropped as today.

### 2.2 Use cases
- **2.2.1 Interface swap.** As a test author, given `@Component RealSink implements
  Sink` and `@TestComponent FakeSink implements Sink`, when I build with
  `--profile=test` and inject `@Inject Sink`, then `FakeSink` is wired and `RealSink`
  is dropped — no `DI_AMBIGUOUS`.
- **2.2.2 Prod unaffected.** Same source, built with `--profile=prod` (or no flag),
  then `RealSink` is wired and `FakeSink` is absent.
- **2.2.3 Own-type double still works.** Given `@TestComponent StubDb` (no
  interface) injected as `@Inject StubDb`, then under `--profile=test` it resolves
  and under `--profile=prod` the inject is unsatisfied (`MISSING_COMPONENT`).
  (Preserves existing behavior.)
- **2.2.4 Multiple real impls.** Given two `@Component`s implementing `Sink` and one
  `@TestComponent` implementing `Sink`, under `--profile=test` both reals are masked
  and the double is the sole `Sink` provider.
- **2.2.5 Profile-scoped double.** A `@TestComponent @Profile("test")` (redundant but
  legal) behaves identically; masking still requires `activeProfile == "test"`.

---

## 3. Shipped annotation declarations

### 3.1 Requirement
`cajeta.aot` ships declared annotation types for the two DI annotations whose
behavior lives in the core substrate — `@Profile` and `@TestComponent` — so user
code has an importable, documented type (parity with `@Component`). Recognition
stays by short-name; the declarations do not change compiler behavior, they document
and stabilize the surface. Framework stereotypes (`@Repository`, …) are out of scope
(§1.4) and belong to primavera.

### 3.2 Use cases
- **3.2.1 Profile.** As a developer, `import cajeta.aot.Profile;` resolves and
  `@Profile("prod")` compiles against a real type.
- **3.2.2 TestComponent.** `import cajeta.aot.TestComponent;` resolves and
  `@TestComponent` compiles against a real type.
- **3.2.3 No regression.** Existing code that used the bare short-name annotations
  with no import continues to compile unchanged.

### 3.3 `@Profile` value cardinality
- **3.3.1** `@Profile` accepts multiple profile names with **any-of** semantics:
  a component is included if any listed profile equals the active profile.
- **3.3.2** Two spellings are supported: repeated `@Profile("dev") @Profile("test")`
  (already captured today) and a single `@Profile("dev", "test")` list form. The
  declaration is `String[] value()`; the visitor reads a string list, falling back
  to the single-string form.

---

## 4. Acceptance

1. `--profile=test` swaps a shared-interface `@TestComponent` in for the real
   `@Component` with no ambiguity; `--profile=prod` uses the real one (§2.2.1–2.2.2).
2. Existing `@TestComponent`/`@Profile` gtests stay green (no regression) (§2.2.3).
3. `import cajeta.aot.Profile;` and `import cajeta.aot.TestComponent;` resolve; the
   annotations compile against declared types; bare-short-name usage still compiles (§3).
4. `@Profile("dev", "test")` includes the component under both profiles (§3.3).
5. A `.cajeta` demo (tour or sample) exercises profile selection and a test-double
   swap end to end via `--emit=exe`.
