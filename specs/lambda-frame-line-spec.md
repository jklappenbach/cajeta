# lambda-frame-line — a lambda frame must carry its source line

## 1. Definition

**1.1** Cajeta renders a semantic call frame as `Package.Type.method(File.cajeta:NN)`.
The `NN` comes from the **line-info shadow stack**: `__cajeta_line_enter` pushes a
frame, `__cajeta_line_mark(line)` updates the top frame at each statement boundary,
`__cajeta_line_leave` pops it. Two surfaces read that stack — the semantic stack
trace printed on an uncaught throw, and the profile trace whose slices the IDE
flame graph makes clickable.

**1.2** A lambda gets its own shadow frame (`<lambda>`), so a callback is named in
its own right rather than folded into the terminal operation that invoked it. That
frame is pushed but **never given a line unless the lambda's body is a block**.
An expression-bodied lambda — `(x) -> f(x)`, the common form — renders `:0`.

**1.3 Measured, 2026-08-31.** Same program, same `-g` build, one difference:

| body form | rendered frame |
|---|---|
| `(x) -> { App.boom(x); }` | `test.App.<lambda>(App.cajeta:10)` |
| `(x) -> App.boom(x)` | `test.App.<lambda>(App.cajeta:0)` |

Every neighbouring frame in the same trace carried a real line
(`App.boom(App.cajeta:12)`, `Stream<int32>.forEach(Stream.cajeta:246)`,
`App.run(App.cajeta:9)`). The lambda frame alone reads 0.

**1.4 Cause.** `Block::generateCode` is the **only** site that emits a line mark.
An expression body is not a `Block`, so no mark ever runs and the frame keeps the
zero it was pushed with. The comment at the frame-push site asserts the opposite —
*"the statement marks the body emits keep the line current"* — which is true for
block bodies and silently false for expression bodies.

**1.5 One fix, both surfaces.** The instrumentation descriptor carries **no line
field at all** (`typeName`, `methodName`, `fileName`, then counters). A profile
slice takes its line from the shadow stack, exactly as a stack trace does, so the
shadow frame is the single point of repair.

**1.6 Why it was never caught.** `StackTraceTextTests` (8 tests) and
`ProfilerEndToEndTests` contain **no lambda**. The frame-push was delivered with a
comment describing the behaviour and no test asserting it — the line was never
measured on either surface, so it has read 0 since the frame was introduced.

**1.7 A second defect, found by the nested-lambda arm.** An inner lambda's frame
renders with an EMPTY declaring class — `.<lambda>(App.cajeta:0)` beside the outer
frame's `test.App.<lambda>(App.cajeta:0)`. The frame is named from the enclosing
METHOD, and a lambda body clears the module's current method before generating its
own body, so a lambda nested in a lambda has none. Same code, same frame push, and
in scope here: a frame that cannot say which class it belongs to is not usable by
either surface.

**1.8 Non-goals.**
- The default (non-`-g`) build, where per-statement marks are off by design and
  every frame reads 0. Only the line-info build is in scope.
- DWARF debug info and breakpoints inside lambda bodies.
- The IDE's behaviour when a line is genuinely absent — already delivered as
  `NavigationOutcome`, and it stays the fallback for frames that have no location.

## 2. The lambda frame's line

**2.1** When a lambda's body is an expression, its shadow frame carries the source
line of that body expression.

**2.2** When a lambda's body is a block, its shadow frame carries a real source
line from the moment the frame is pushed, before any statement in the block has
run.

**2.3** When a statement inside a block-bodied lambda runs, the frame's line
advances to that statement, as it does for an ordinary method body.

**2.4** When a lambda is nested inside another lambda, each frame carries its own
line and the two are distinguishable.

**2.5** When two lambdas appear in one method, their frames carry different lines.

**2.6** When a lambda's body expression begins on a different line from the
lambda's parameter list, the frame reports the line the body begins on.

**2.7** When a lambda is nested inside another lambda, its frame names the
declaring class, exactly as the enclosing lambda's frame does.

**2.8** When the build has no line info, a lambda frame reports 0, as every other
frame does — the fix must not smuggle a line into a build that opted out.

## 3. Surfaces

**3.1** When an uncaught throw passes through an expression-bodied lambda, the
printed trace names that lambda with a positive line.

**3.2** When a profiled run records an expression-bodied lambda, the trace's slice
for that lambda carries a positive line.

**3.3** When the IDE flame graph opens a lambda frame from such a trace, it
navigates to the lambda's own line rather than the top of the file.

## 4. Coverage

**4.1** When a lambda's line is asserted, the assertion distinguishes "the frame is
absent" from "the frame is present with line 0" — a check that passes on a missing
frame cannot detect this defect.

**4.2** When the expression-body arm is asserted, a block-body arm is asserted
beside it, so removing marks entirely fails one and reverting the fix fails the
other.

**4.3** When the fix is reverted, at least one test fails on each surface named in
§3.
