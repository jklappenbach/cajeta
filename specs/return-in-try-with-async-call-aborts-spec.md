# return-in-try-with-async-call-aborts — `return` inside a `try` containing `connectAsync` aborts the compiler (defect spec)

> Status: **draft**, filed 2026-10-25 by the dev.cajeta.bench author from a
> standalone probe on v0.46.3.

## 1. Definition
```cajeta
try { ... connectAsync(...) ...; return x; } catch (Throwable e) { return y; }
```
aborts the compiler with `terminate called after throwing an instance of
'char const*'`. Without the `return` inside the `try` the same code
compiles. A compiler abort is never acceptable output; the worst case is a
diagnostic by name.

## 2. Use cases
- **2.1** As a developer, when I return from inside a `try` that awaits an
  async call, then the code compiles and the return unwinds the try scope
  (and any suspended frame) correctly.
- **2.2** As the test suite, then a compiler test covers the shape, and the
  thrown `const char*` is replaced by a proper diagnostic wherever it comes
  from, so no input can terminate the compiler through it.

## 3. Acceptance
The probe compiles and runs; `grep -rn 'throw "' src/cajeta` finds no bare
string throws on a reachable path.
