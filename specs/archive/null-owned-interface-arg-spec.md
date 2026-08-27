# null-owned-interface-arg — runtime SIGSEGV passing `null` to a `#`-interface formal

## 1. Definition (defect)

Passing the literal `null` as an OWNED interface-typed argument
(`#Transformer a` formal, call site passes `null`) compiles but SIGSEGVs at
runtime (fault addr 0x10 — a vtable-half dereference on a null fat
pointer), at or before the receiving constructor stores it with `#=`.
Found 2026-07-31 building cajeta-ml's `Pipeline`: `heap Pipeline(null,
null, null, 0, #fin)` crashed inside `Pipeline.of` on the v0.12.1-dev
toolchain; padding the slots with real `IdentityTransformer` instances
fixed it (the shipped workaround, commented at the site).

## 1a. Mechanism (narrowed 2026-07-31)

Interface-typed params cross the call boundary as `ptr` TO the 24-byte fat
struct `{data, vtable, kind}` (CajetaFunctionType::toCallingConvType /
Method::generatePrototype; see MethodCallExpression's
`spillAggregateForByPointerArg`). A literal `null` argument lowers to a raw
null POINTER — the callee then copies the fat struct from address 0
(`this.f #= f` reads null+16 for `kind` → the observed fault 0x10). Fix
shape: at argument coercion, a null constant bound to an interface formal
must spill a ZEROED fat struct and pass its address (matching what a null
interface FIELD read produces), plus the transfer/drop paths treating an
all-null fat value as a no-op.

## 2. Requirements

- 2.1 Minimal repro: interface `I`, class with `#I` ctor formal, call with
  `null`, store with `#=`, drop.
- 2.2 Decide + implement the contract: either a null owned-interface value
  is legal (fat pointer `{null, null}`, transfer and drop are no-ops —
  preferred, matches nullable owned class fields like `DynFrame.backing`)
  or passing `null` to a `#`-interface formal is a compile-time error.
  Silent runtime corruption is the only wrong answer.
- 2.3 Regression pins either way.
- 2.4 Ships with cajeta v0.13.0 (cajeta-ml-v2 plan U7).

## 3. Status

FIXED 2026-07-31 (requirement 2.2, "legal empty value" option):
`CajetaClass::invokeMethod`'s argument coercion spills a ZEROED 24-byte fat
body for a null constant bound to an interface formal and passes its
address — the canonical all-null fat value a never-assigned interface field
reads as. Pin: `PlaceholderOwnedFieldTests.nullIntoOwnedInterfaceFormal`
(live). Out of scope, noted: `f == null` on an interface-typed field has no
fat-aware compare lowering — nulls are observable only via dispatch guards;
a fat-aware `==` is a candidate follow-up.

The `Pipeline` IdentityTransformer padding stays (it is also semantically
cleaner than nullable stages).

---

**CLOSED — verified fixed on cajeta 0.14.0 (8ca5b362), 2026-08-01.** Re-ran this
spec's repro against a freshly built 0.14.0 compiler; the defect no longer
reproduces. Archived per td-project-workflow (spec -> archive, INDEX row dropped).

## 4. Follow-up closed too — fat-aware `==` (2026-08-03)

The candidate follow-up §3 records ("`f == null` on an interface-typed field
has no fat-aware compare lowering — nulls are observable only via dispatch
guards") is now DONE, so this spec has nothing left open.

Confirmed first, red-first, as
`PlaceholderOwnedFieldTests.nullInterfaceIsObservableViaEquals`: a null
interface compared `false` against null and a real one also compared "not
empty", so the predicate was simply never true.

Cause: both interface shapes hand back a pointer to the 24-byte
`{ ptr data, ptr vtable, i64 kind }` BODY — a field's GEP *is* the body
address, a local's slot loads to it — so `== null` compared the body's
ADDRESS, which is never null. Fix in `BinaryOpExpression.cpp`'s EQ/NE arm:
when one operand is interface-typed and the other is a null pointer constant,
load and compare the body's DATA word. The null case memsets the body to
zero, so `data == null` is exactly "empty".

Landed alongside the `owned-interface-return-fault` fix (same session); both
pinned in `PlaceholderOwnedFieldTests`, 10/10.

## 5. Residue closed — the two shapes §4 did not reach (2026-08-27)

§4's fix loaded the body's DATA word whenever an operand arrived as a POINTER
to the 24-byte fat body. Two shapes do not arrive that way, and both were still
broken:

- **A STATIC field** loads as the fat VALUE, not a pointer, so the
  interface-vs-null block never fired at all and the struct reached
  `CreateICmp` against a null pointer — `ICmpInst::AssertOK(): Both operands
  to ICmp instruction are not of the same type!`, a COMPILER crash. Now the
  value shape takes its data word via `extractvalue`.
- **An unbound local** (`Sink s = null; if (s == null)`) — the shape §4
  assumed already worked. The pointer to the body is itself null when the
  reference was never bound, and the data-word load ran unconditionally:
  SIGSEGV, fault addr (nil). The compare now branches — a null pointer IS a
  null reference — and only a live one gets its data word read, joined by a phi.

Fixed in `2a689d4f` (BinaryOpExpression.cpp), pinned by
`InterfaceStaticNullCompareTests`. Both directions are pinned deliberately: a
fix that always answered "null" would satisfy a one-sided test while breaking
every guard written this way. The instance-field and bound-local shapes §4
fixed get their own controls so this cannot regress them.

Found writing an opt-in diagnostics hook for cajeta-llm — "a static sink field,
null when nobody is listening" — the most obvious way to write that shape, and
it was unwritable. The second defect was found only because the control test
for "the shapes that already work" was written; see [[controls-must-vary-the-mechanism]].

**This spec is now closed for real**, and its INDEX row is dropped.

## 6. The root cause, found after §5 was written (2026-08-27)

§5 credited `2a689d4f` with closing the residue. It closed the SYMPTOMS. The
cause was underneath, and is the more serious half: `getOrCreateStaticFieldGlobal`
declared every non-primitive static as `ptr`. Right for a class, whose instances
are referenced by pointer; wrong for an interface, whose value IS the 24-byte fat
struct, stored and loaded BY VALUE. The global was under-allocated by 16 bytes.

    @pkg.Main.direct = global ptr null
    call void @llvm.memcpy(ptr @pkg.Main.direct, ptr %8, i64 24)
    %12 = load %pkg.I, ptr @pkg.Main.direct

Unoptimized builds tolerate the overrun and read back what was written — which is
why every JIT test added alongside §5's compare fix passed. Under `--release`,
LLVM knows the object is 8 bytes, folds the out-of-bounds reads, and **an
ASSIGNED static interface field compares EQUAL TO NULL**. A silent miscompile, in
the shipping configuration only.

Fixed in `0d2a40c4`. Surfaced only because cajeta-llm's suite builds twice, the
second pass under `--release --live-set=bounded`; only the second failed.

**The lesson worth keeping**, in that commit's own words: had it shipped, §5's
compare fix "would have turned a loud compiler crash into a quiet wrong answer,
which is the worse trade." A fix that removes a crash without finding what the
crash was reporting can make the defect harder to see, not gone. See
[[controls-must-vary-the-mechanism]] — here the missing control was the
configuration, not the code path: a debug-only test suite could not observe this.

