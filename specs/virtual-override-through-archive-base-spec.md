# virtual-override-through-archive-base — an override in a `.cja` base class is not reached from a subclass that overrides a sibling method (defect spec, unconfirmed)

> Status: **draft, unconfirmed**, filed 2026-10-25 from the cajeta-codec bench
> on cajeta 0.46.3.

## 1. Definition
In `cajeta-codec/bench`, three subclasses of an intermediate class that
itself overrides `runVariant(variant, iterations)` (from `dev.cajeta.bench`'s
`Benchmark`, loaded from a `.cja`) overrode only `run(iterations)`; their rows
reported nothing, meaning the intermediate `runVariant` override was never
dispatched to. Moving the bodies into a new `work()` method fixed it. A
same-module reproduction dispatched correctly, so the suspicion is a vtable
layout or override-resolution difference when the base class comes from an
archive. Unconfirmed: the bench author had no time to isolate it.

## 2. Use cases
- **2.1** As a developer, when a class hierarchy spans an archive boundary,
  then virtual dispatch resolves exactly as it does within one module.
- **2.2** As the test suite, then a test builds a base with two virtual
  methods into a `.cja`, subclasses it twice (one overriding each method)
  from a consumer, and asserts both dispatch paths; and the codec bench's
  `work()` indirection is removed once it passes.

## 3. Acceptance
The reproduction in 2.2 passes or the suspicion is retired with a measured
explanation.
