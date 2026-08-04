# owned-interface-return-fault — `#Interface` return faults on first use — FIXED 2026-08-03

## 0. Resolution (2026-08-03)

**FIXED**, and the defect was far broader than §1 describes. The container,
the field, and the second frame are all irrelevant: **every `#Interface`
return of a concrete class was miscompiled.** The minimal repro is 15 lines
with no container anywhere —

```cajeta
public static #Face make(int32 k) { return heap Plus(k); }
Face f = D.make(10);
return f.poke(5);           // SIGSEGV
```

pinned as `PlaceholderOwnedFieldTests.PROBE_ifaceReturnToLocalDispatch`. §2's
"ruled out" list held up — each item really does work in isolation — but it
led away from the answer, because nobody tried the return on its own.

**Root cause.** An interface value is a 24-byte `{ ptr data, ptr vtable, i64
kind }` returned BY VALUE, and every other way of producing one assembles that
body. `ReturnStatement::generateCode` had no wrap, so `return heap Plus(k)`
fell into the general coercion arm `retTy->isAggregateType() && valTy->isPointerTy()`
— "the expression yielded the ADDRESS of the aggregate, so load it". That is
correct for an `@ValueType` (`return stack Vec2(...)`) and catastrophic here:
the pointer is the Plus OBJECT, not a body. The emitted IR read 24 bytes out
of a 16-byte object:

```llvm
%2 = call ptr @malloc(i64 16)      ; Plus is {vtable, k}
%4 = load %test.Face, ptr %2       ; reads 24
ret %test.Face %4
```

so the caller received `data` = Plus's vtable pointer, `vtable` = Plus's `k`
field, and `kind` = 8 bytes of heap past the allocation. Dispatch loaded
through `k + 8`; with `k = 10` that is address **0x12** — the exact fault the
repro reports, which is how the diagnosis was confirmed rather than assumed.

**Fix.** `Statement.cpp` — wrap a concrete class into the interface's fat body
BEFORE that coercion arm can misread it (data = the object pointer, vtable =
`srcCls->getInterfaceVTable(iface)`, kind = OWNED for a `#` return / fresh
construction / explicit `#x`, BORROWED otherwise), then return the body struct
by value. Verified at the IR level, not just by test outcome.

**§4.3 answered.** The `#<BaseClass>` control PASSES while the interface case
faulted, so the defect is fat-value handling, not return-slot lifetime. Both
are now pinned (`ownedBaseClassReturnSurvivesContainerField`).

**Downstream.** cajeta-ml's `SpelaTrainer` workaround (build each optimizer
inline, §5) can be reverted. `inlineInterfaceBuildIsTheWorkingBaseline` pins
that the workaround keeps working either way, so the revert is optional.

## 1. Definition (defect, as originally reported)

A method whose return type is `#<Interface>` — ownership transfer of a
FAT interface value — hands back a value that faults (SIGSEGV, `fault
addr (nil)`) the first time it is used, when the caller stores it into a
container field and reads it back later.

Observed 2026-07-31 building cajeta-ml v3 U7 (`ml/train/SpelaTrainer`):

```cajeta
#Optimizer makeOptimizer(Module layer) {          // Optimizer is an interface
    if (...) { return heap SGD(layer.parameters(), lr, momentum); }
    return heap AdamW(layer.parameters(), lr, weightDecay);
}

// in the constructor:
Optimizer opt = this.makeOptimizer(this.layers.get(i));
this.optimizers.add(#opt);                        // ArrayList<Optimizer> field
...
// later, in another method:
Optimizer opt = this.optimizers.get(i);
opt.setLr(lr);                                    // SIGSEGV here
```

Replacing the helper with the SAME allocations written inline in the
constructor — no `#Interface` return anywhere — makes the fault vanish
and the code behave correctly:

```cajeta
Module lm = this.layers.get(i);
if (...) {
    Optimizer o = heap SGD(lm.parameters(), lr, momentum);
    this.optimizers.add(#o);
} else {
    Optimizer o = heap AdamW(lm.parameters(), lr, weightDecay);
    this.optimizers.add(#o);
}
```

## 2. What was ruled out

Each of these was probed in isolation and PASSES, so none is the cause:

- `ArrayList<Interface>` as a container — add + get + method call works.
- A variable (non-constant) index into that container works.
- Storing an interface built from `heap Concrete(...)` and calling
  through it works.
- `ArrayList.add` / `.set` ownership — both are dual-capable
  (`Cajeta.owned(v)`, `#=` vs `=`) and handle `#v` correctly.
- The whole surrounding algorithm (tape, forward, loss, backward,
  `gradsOf`, `opt.step`) works inline in one function.

The single remaining difference between the crashing and working forms is
whether the interface value passed through a `#Interface` RETURN.

## 3. Suspected cause

An interface value is a fat (24-byte) body. The `null → #interface
formal` defect fixed 2026-07-31 concerned the same representation on the
ARGUMENT side (a zeroed fat body had to be spilled for the call). The
RETURN side plausibly has the mirror gap: the fat body is returned by
value into a temporary whose lifetime ends before the caller's `#`
transfer copies it, so what lands in the container is a pointer into a
dead frame — consistent with a null/garbage vtable on first dispatch.

## 4. Requirements

- 4.1 A `#<Interface>`-returning method must hand back a value that stays
  valid after the caller stores it — including into a container field
  read back in a later call.
- 4.2 Repro pin: the shape in §1 (interface-returning factory → container
  field → method call through the retrieved element), reduced to a
  standalone test.
- 4.3 Check the SUPER-type variant too: a `#<BaseClass>` return of a
  derived instance, which shares the "return type wider than the
  allocated type" property but not the fat body — that tells us whether
  the bug is fat-value handling or return-slot lifetime.
- 4.4 Related: `null-owned-interface-arg` (FIXED, argument side) and
  `field-store-title-trap` (plain formal → plain field store).

## 5. Workaround

Build the value inline at the site that stores it, or return a CONCRETE
type and widen at the assignment. In cajeta-ml the constructor builds
each optimizer inline; the code is slightly longer and the comment says
why, so the workaround does not read as arbitrary.
