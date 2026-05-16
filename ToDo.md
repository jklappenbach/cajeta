# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Up next

### Iterator + Optional standard library

Design + implement `Optional<T>` (value-typed sum) and `Iterator<T>` (interface + a couple of canonical struct implementers), then wire the `for (x in coll)` desugaring through them.

**Status:** in design. Structs.md has sketches at lines 155–176 (Iterator) and 356–393 (Optional) but the deeper conversation about lifetimes, dispatch trade-offs, for-loop desugaring shape, error-flow integration, and stdlib placement was not written up. Next step is a dedicated design doc (`cajeta-docs/IteratorOptional.md` or split) capturing those decisions before any code.

**Why now:** unblocks the for-loop ergonomics promised in `Structs.md` and is the natural first real consumer of S9–S11's interface fat-pointer + dyn-dispatch machinery. Also the missing-stdlib gap a polymorphic-iteration test in S11 worked around (`dispatchInPolymorphicSequence` is a workaround for what should be `for (s : iter)`).

**Touches:**
- `runtime/src/cajeta/lang/Optional.cajeta` (new)
- `runtime/src/cajeta/lang/Iterator.cajeta` (new)
- `runtime/src/cajeta/collection/ArrayIter.cajeta` (new — canonical struct impl over `T[]`)
- For-loop desugaring in the visitor (`CajetaLlvmVisitor.h::visitEnhancedFor` or similar — confirm during design)
- Tests across `test/parser/Optional*`, `test/parser/Iterator*`, possibly extending `EnhancedForTests`

---

## Carried-over deferreds from the struct/view rollout

Bugs and gaps known at the close of Session 12; each was small enough to keep the rollout green but worth fixing once a real consumer hits it.

### S6.1 — `Foo(args)` constructor-call syntax on a struct segfaults

**Repro:** any struct used with constructor-call syntax (`Header(bytes)`) instead of aggregate-init (`Header { ... }`). Pre-S6.1 the rejection came from the struct prototype itself throwing; with that gone, the call enters method-call dispatch which assumes a class receiver and trips a null deref.

**Fix shape:** guard at the dispatch site that recognizes a `CajetaStruct` receiver and throws `CAJETA_ERROR_STRUCT_NO_CTOR` (or routes to aggregate-initializer parse).

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S6.1 limitations called out".

---

### S7.4 — Move out of struct field doesn't clear runtime pointer (double-free risk)

**Repro:** `Tag stolen = #w.h.t;` marks the path moved at compile time, but does NOT clear the field's pointer slot at runtime. The class drop's recursive struct-drop walks the now-stale pointer and double-frees on scope exit.

**Fix shape:** struct's runtime drop fn needs to consult a per-instance ownership bitmap, OR the move-out point needs to write null into the slot. Touches the same per-instance ownership-tracking gap noted under S6.4.

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S7.4 limitations called out".

---

### S7.5 / S10.5 — Class-array element-layout ambiguity (blocks polymorphic interface arrays)

**Repro:** `Cell[] cells` where `class Cell { Point pt; }` misreads. `CajetaArray::getElementLlvmType` returns the full class LLVM struct, but the read path treats slots as 8-byte pointers. Without an embedded struct the mismatch happens to read the vtable pointer as a class reference (almost-right); with an embedded struct after the vtable, the indirection picks up the wrong bytes entirely.

**Same root blocks:** true `Greeter[]` polymorphic interface arrays (S11.5's `dispatchInPolymorphicSequence` test had to use individual locals instead of an array).

**Fix shape:** needs a design call — class/interface arrays store inline values or pointer references? Pick one, then align `getElementLlvmType` + the read path + the write path + element drops consistently.

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S7.5 limitations called out".

---

### S8.4 — Direct chaining on a struct-returning method segfaults

**Repro:** `p.shift(10, 20).shift(100, 200)`. The intermediate return value gets wrapped into a fresh body alloca by `MethodCallExpression`'s S6.7 repackaging, but using that wrapped pointer as a receiver for the next chained call doesn't line up cleanly — the inner call's drop-chain interaction and the outer call's receiver-load conflict. Works fine when bound to a local first.

**Fix shape:** `MethodCallExpression`'s receiver-handling needs to recognize an aggregate value (vs. a stable l-value) as a callable receiver.

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S8.4 limitations called out".

---

### S5b — `.length()` on T[] returned from a view trips an unrelated alloca path

**Repro:** reading a T[] field from a view, then calling `.length()` on the result, hits an LLVM `dyn_cast<AllocaInst>` on a non-present value. Tests work around by indexing instead.

**Fix shape:** focused look at the array-length codegen path; unrelated to S5b proper but surfaced by S5b's view-T[] tests.

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S5b limitations called out".

---

### S5b — Variable-size nested views not detected as variable-size

**Repro:** a view field whose type is another view that itself has var-size fields — `CajetaAggregate::isVariableSize` checks for String / CajetaArray, not "view containing var-size", so nested var-size views skip the length-prefix validation sweep at construction.

**Fix shape:** extend `isVariableSize` recursively, then have the construction-time validation pass walk into nested var-size views.

**Lives in:** see `cajeta-docs/history/StructsViewsStatus.md` § "S5b limitations called out".

---

## Done

(empty)
