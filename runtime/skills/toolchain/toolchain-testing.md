---
id: toolchain-testing
applies-to: [cajeta/toolchain/testing, cajeta/toolchain/test]
title: Writing and running tests with cajeta-unit
description: cajeta-unit is an external library dependency (dev.cajeta.unit), not built into the toolchain — how to declare it, write tests, and wire the `test` task so a failure fails the build.
---

# Testing — cajeta-unit

**There is no built-in test framework.** `cajeta test` runs a
*manifest-declared task*, nothing more; the assertions come from
**cajeta-unit**, an ordinary library dependency published as
`dev.cajeta.unit` (own repo: `jklappenbach/cajeta-unit`, Apache-2.0). Without
declaring it and wiring a task, `cajeta test` has nothing to run.

## Writing tests

A test class is ordinary cajeta with a `static` entry that builds a
`TestRunner`, registers closures, and returns the summary — **`0` green,
`1` any failure**, which is what fails the build:

```cajeta
import dev.cajeta.unit.Assert;
import dev.cajeta.unit.TestRunner;

public class MathTests {
    public static int32 run() {
        TestRunner t = heap TestRunner();

        t.test("two plus two", () -> {
            Assert.that(2 + 2).isEqualTo(4);
        });
        t.test("a name is shaped right", () -> {
            Assert.that("o_42").startsWith("o_");
            Assert.that("o_42").hasLength(4);
        });
        t.test("charging a negative amount is rejected", () -> {
            Assert.assertThrows(() -> service.charge(-1));
        });
        t.skip("flaky timeout", "see #412");

        return t.summary();   // prints the report; 0 = green, 1 = any failure
    }
}
```

Annotation-driven `@Test` discovery (v0.3) and a Mockito-style AoT mock engine
(v0.4 — `when/thenReturn`, argument matchers, `verify(times/atLeast/atMost)`,
argument capture, in-order verification over hand-written mocks) also ship.

## Assertions

| Subject | Entry point |
|---|---|
| integers | `Assert.that(int64)` — `isEqualTo` `isGreaterThan` `isZero` `isPositive` … |
| strings | `Assert.that(String)` — `isEqualTo` `contains` `startsWith` `endsWith` `hasLength` … |
| floats | `Assert.thatFloat(float64)` — `isCloseTo(value, tolerance)` … |
| booleans | `Assert.isTrue(cond)` / `Assert.isFalse(cond)` |
| any | `Assert.fail(msg)` · `Assert.assertThrows(() -> ...)` |

**Why the distinct names**: `Assert.thatFloat` and `Assert.isTrue` exist
because an integer literal in `that(1)` would otherwise mis-bind to a
`that(float64)` / `that(boolean)` overload — the numeric-literal overload
hazard from `cajeta/language/types`. A failing check throws
`AssertionFailure`; the runner catches it, marks the test failed, prints why,
and continues.

## Wiring the build

Declare the dependency under `dev-dependencies`, then make the `test` task
build a runner binary whose entry is your test class and run it:

```jsonc
// cajeta.json
"test": {
  "actions": [
    { "action": "build", "flavor": "debug",
      "entry-method": "your.pkg.Tests.run", "id": "testbin" },
    { "action": "test", "input": "${testbin.path}" }
  ]
}
```

- `cajeta test` — builds + runs the tests; **a non-zero exit fails the build**.
- `cajeta build` — build only; **tests are skipped**.

The default `cajeta init` archetype ships a heavier `test` task that also runs
coverage instrumentation (`cajeta.coverage.instrument`, min/min-per-file gates,
console/html/sarif reports) — keep or trim it, but keep the
build-then-`action: test` pair.

## What is NOT available

`@BeforeAll` / `@AfterAll` / `@Tag`, auto-generated `@Mock` subclasses (needs a
compiler codegen hook), spies, environment fakes, and JUnit-XML / TAP reporters
are roadmap, not shipped. `@BeforeEach` / `@AfterEach` / `@Disabled` are.

For DI-heavy code, the `test` profile (`--profile=test`) enables the `@Inject`
override seam used to substitute mocks (`cajeta/language/annotations`).
