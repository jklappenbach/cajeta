# Interface Methods with Generic-Instantiation Return Types — Defect Spec

> Status: **FIXED 2026-07-30** (same day as discovery). Root cause: interface
> method declarations FORCED the sret return ABI for every class return
> except literal `cajeta.lang.String` (`Method.cpp returnsStackValue()`,
> the #63 heuristic), while implementations returning ordinary heap classes
> emit `ret ptr` — the indirect call through the interface vtable misaligned
> (the sret slot became `this`) and dispatch SIGSEGV'd. The fix flips the
> exception into a whitelist: sret is forced ONLY for the value-shape-by-
> convention return (`cajeta.lang.Optional`, the AsyncIterator.next shape —
> ecosystem-wide the only one); every other class return takes the reference
> (pointer) ABI, matching what impl bodies emit. Diagnostics added per §2.3
> (null vtable slots and dispatch-index misses now warn instead of silently
> nil-calling). Regression pins: `InterfaceTests` gained 4 tests covering
> plain-generic, owned-`#`-generic, String, and Optional returns through
> interfaces; the cajeta-ml pin test is un-gated and green.
>
> CORRECTION: "manifestation 1.2" below was a false alarm — the repro
> imported `cajeta.lang.ArrayList` (it lives in `cajeta.collection`); the
> placeholder error was the (rough) wrong-import diagnostic, unrelated to
> interfaces. Historical text below kept for the record.

## 1. Definition

An interface method whose **return type is a generic class instantiation**
(`Tensor<float64>`, `ArrayList<int32>`, owned `#` or plain) misbehaves when
dispatched through an interface-typed receiver. Two manifestations, one
family:

- 1.1 **Runtime null-slot call (SIGSEGV, fault addr nil).** When the
  instantiation exists elsewhere in the program (e.g. `Tensor<float64>` used
  by the caller), the program compiles, but calling the method through the
  interface jumps through a null pointer. `fit(Tensor, Tensor) → void`
  through the same receiver works — **param-position generics are fine;
  return-position generics are not.** Non-generic returns (`#String`,
  `#int8[]`, primitives) dispatch correctly.
- 1.2 **Compile-time placeholder failure.** When the interface signature is
  the ONLY mention of the instantiation (`#ArrayList<int32> list()`),
  compilation fails: `CAJETA_ERROR_UNRESOLVED_PLACEHOLDER: unresolved
  forward reference to type 'ArrayList'` — the interface signature never
  forces the template instantiation. Same lifecycle family as the
  synth-class first-instantiation loader gap (nucleo-frame 16.1.1 blocker).

### 1.3 Minimal repro (manifestation 1.1)

```cajeta
public interface Speaker { Tensor<float64> peek(); }
public final class Dog implements Speaker {
    private Tensor<float64> held;
    public Dog(Tensor<float64> t) { this.held #= t.copy(); return; }
    public Tensor<float64> peek() { return this.held; }
}
// caller:
Speaker s = heap Dog(y);          // y: Tensor<float64>
Tensor<float64> p = s.peek();     // SIGSEGV, fault addr (nil)
```

### 1.4 Where the machinery goes wrong (initial findings)

- `CajetaClass::synthesizeInterfaceVTables` (CajetaClass.cpp ~1776) matches
  interface methods to concretes by name over
  `getFlattenedInterfaceMethods()`; a missing/unbuilt concrete leaves a
  **silent null slot**.
- The dispatch site (CajetaClass.cpp ~6444) finds `methodIdx` by name in the
  same flattened list; if the method is absent, `callee` silently keeps its
  prior value. Either path ends in a null-pointer call with **no
  diagnostic**.
- Root family: interface method signatures do not demand/force generic
  template instantiation for their return types (params evidently do), so
  the method never resolves into the interface's effective method list
  and/or the concrete's LLVM function is absent at vtable-synthesis time.

## 2. Requirements

- 2.1 Interface methods may declare generic-instantiation return types,
  owned (`#T<A>`) and plain, and dispatch correctly through interface-typed
  receivers — parity with class virtual dispatch.
- 2.2 Interface signatures force-instantiate the generic types they
  reference (return AND param positions), eliminating manifestation 1.2.
- 2.3 **No silent null slots**: a vtable slot that cannot be filled is a
  compile-time diagnostic naming the class, interface, and method — never a
  runtime nil call.
- 2.4 Un-gate the pinned consumer test: cajeta-ml
  `ProtocolTest.predictorIsUsableThroughTheInterface` (@Disabled with this
  spec cited), plus a compiler-repo JIT regression test derived from §1.3.

## 3. Consumers blocked

- `cajeta-ml`: the estimator protocol's core promise (`Predictor.predict`
  through the interface); `crossValScore` (plan Unit 3) is [~] until fixed.
- Any future protocol-style interface returning stdlib generics (streams of
  tensors, table-returning services).
