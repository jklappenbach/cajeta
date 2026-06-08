# Unit test framework plan ("CUnit" + Mocks + Fakes)

Source: `plans/current-focus.md` → **Unit Test Fx** (CUnit, Mocks, Fakes).

> **Status at authoring:** nothing exists. There is **no** `cajeta.test` /
> `cajeta.testing` stdlib package — no assertions, no test runner, no
> mock/fake support anywhere in `runtime/`. This plan builds it from scratch.
> The compiler's own correctness is covered by the C++ gtest suite under
> `test/`; this framework is for **Cajeta user code** to test **Cajeta**.

This is a **scaffold** — the design space is wide and several load-bearing
decisions are Julian's to make (see *Open decisions*). It is intentionally light
on prescription until those are settled. No checkboxes are "ready to build" yet;
the first deliverable is a settled design.

## What "done" looks like

A Cajeta developer can:
- [ ] Write a test class/method, assert on values, and run it.
- [ ] Get a clear pass/fail report (count, failures with file:line + message).
- [ ] **Mock** a dependency (record/verify interactions, stub return values).
- [ ] Provide a **Fake** (a hand-written lightweight implementation) and inject
      it in place of the real collaborator.
- [ ] Run the suite from the build tool (`cajeta test` already exists as a native
      action — `plans/buildtool/build-tool-plan.md`; this framework is what it
      should invoke for Cajeta tests).

## Components

1. **CUnit** — the core: assertion library + test discovery + runner + report.
   - Assertions: `assertEquals`, `assertTrue`, `assertThrows`, `assertNull`,
     float tolerance, collection equality, etc.
   - Discovery: how tests are identified (annotation? naming convention?
     base class? — see D1).
   - Report: console summary; machine-readable output for the build tool / IDE
     (ties into `plans/compiler` curses output and the IntelliJ test runner UI).
2. **Mocks** — interaction-based test doubles. Needs a mechanism to synthesize a
   stand-in for an interface/class that records calls and returns configured
   values. Heavily dependent on the reflection / codegen surface (see D3).
3. **Fakes** — working lightweight implementations the developer writes; the
   framework's role is mainly **injection** (swapping the fake for the real impl
   in the system under test). Ties into the existing aspects/DI surface
   (`AspectsDiDemo` in the tour, `ComponentInjectMethod` in the compiler).

## Open decisions (need Julian) — these gate the build

- **D1. Test declaration style.** Annotation-driven (`@Test` / `@BeforeEach`,
  JUnit-shaped) vs convention (methods named `test*`) vs a `TestCase` base class
  with overridden methods? Cajeta already has rich annotations (`@Builder`,
  `@Encoding`, `@Kernel`), so `@Test` is the natural fit — confirm.
- **D2. Written in Cajeta or native?** The installer plan notes `cvm` is written
  in Cajeta (dogfooding). Same question here: is the framework a pure-Cajeta
  stdlib package, or does the runner need native/compiler support (e.g. test
  discovery via a compiler pass)? Strong lean: **pure Cajeta**, runner is a
  generated `main` — but mocks may force compiler help (D3).
- **D3. Mock synthesis mechanism.** This is the hard one. Options:
  - *Reflection at runtime* — needs the reflection surface (gated on the same
    work as the tour `ReflectionDemo`).
  - *Codegen / annotation processor* — `@Mock`-driven synthesis of a stand-in
    type at compile time (like `@Builder`).
  - *Manual* — no auto-mocks; users hand-write fakes only (cuts the Mocks
    component entirely; simplest).
  Pick determines whether "Mocks" is v1 or deferred.
- **D4. Assertion failure mechanism.** Exceptions (`throw` caught by the runner)
  vs a non-throwing recorder? Cajeta has `try/catch/throw` (`ErrorsDemo`), so
  exception-based is natural.
- **D5. Build-tool integration shape.** `cajeta test` is already a native action
  — does it shell out to a compiled test binary, or is there a deeper protocol
  (per-test results streamed for the IDE)? Coordinate with the IntelliJ plugin
  test-runner story.

## Dependencies / coordination

- Mocks (D3) may block on the **reflection** surface — coordinate with
  `plans/tour/tour-refactor-plan.md` D3 (same unknown).
- Report format coordinates with `plans/compiler/curses-output-plan.md` (shared
  notion of structured, streamable results) and the IntelliJ plugin.
- Fakes injection coordinates with the existing DI/aspects machinery.

## First step

- [ ] Resolve D1–D3 with Julian, then write the actual phased, checkbox-tracked
      build plan (mirroring `build-tool-plan.md` / `cajeta-net-plan.md`
      discipline: test-first, stable ids, acceptance criteria per phase).
