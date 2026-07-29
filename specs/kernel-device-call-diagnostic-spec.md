# Silent kernel drop — @Kernel calling non-@Device code — spec (draft)

Origin: docs-refactor 15.9 (unit-12 xpu tour, 2026-07-03).

## 1. Definition

A `@Kernel` method that calls into a class not annotated `@Device` (e.g.
`cajeta.xpu.Sdf`) compiles with **no diagnostic**; the kernel silently
never registers, and the launch fails at runtime with "no registered CPU
kernel". The failure is far from the cause and reads like a scheduling
bug, not the source-level rule violation it is.

## 2. Features

### 2.1 Compile-time diagnostic at the call site
When lowering a `@Kernel` body, a call that resolves to a method of a
non-`@Device` class raises a compile-time error naming the callee, its
class, and the fix (`@Device`-annotate the class, or keep device code
device-only). Severity: error by default (silent drop is never wanted);
consider a lint escape hatch only if a real mixed-mode use case appears
at plan time.

Use cases:
1. As an xpu developer, when my kernel calls a helper class I forgot to
   annotate, then the compile fails at that call — not a runtime "no
   registered CPU kernel" hours later.
2. As the tour's xpu chapter, when a demo shows the rule, then the error
   text is the teaching aid.

### 2.2 @Device-annotate `cajeta.xpu.Sdf`
`Sdf` is device-safe math and the tour README already lists its
annotation as pending — annotate it so SDF kernels compile out of the box.

Use cases:
1. As a tour reader, when I write an SDF kernel from the README example,
   then it compiles and registers without touching stdlib source.

## 3. Non-goals
Automatic transitive `@Device` inference (explicit annotation is the
contract); device-side stdlib expansion beyond Sdf.
