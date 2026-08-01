# owned-interface-return-fault — `#Interface` return faults on first use (draft)

## 1. Definition (defect)

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
