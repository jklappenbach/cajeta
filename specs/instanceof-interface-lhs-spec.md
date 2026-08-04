# instanceof-interface-lhs — defect

Found 2026-08-04 implementing `stdlib-completion` U1 (`cajeta.math.distance`).

## 1. Defect

`expr instanceof ConcreteClass` **SIGSEGVs at runtime** when `expr`'s static
type is an **interface**. Nine-line repro (JIT and suite both):

```cajeta
package test;

import cajeta.math.distance.Euclidean;
import cajeta.math.distance.Metric;

public final class R {
    public static int32 main() {
        Metric e = heap Euclidean();
        if (e instanceof Euclidean) { return 1; }   // SIGSEGV here
        return 2;
    }
}
```

Fault address is a small offset (observed 0x29/0x49/0x59/0x79 across runs),
i.e. a field read through a garbage base pointer.

## 2. Analysis (from the session that found it)

`InstanceOfExpression::generateCode` (src/cajeta/asn/expression/Expression.cpp)
computes `wantRuntime` from the lhs's static type: wildcard, same generic
base, or **class**-typed lhs route to the runtime RTTI check
(`__cajeta_instanceof_named`). Two suspected gaps for an interface-typed lhs:

- **2.1** An interface value is a **fat pointer** (object + itable — the
  `owned-interface-return-fault` family). Whatever path the lhs takes here
  hands the RTTI (or the fold) a pointer that is not the plain object
  pointer, and the class-descriptor read at a small offset faults.
- **2.2** If an interface-typed lhs instead folds to a compile-time
  constant, the fold is a **lie** (`e instanceof Euclidean` on a value that
  IS a `Euclidean` must be true) — the same shape as the pre-nucleo-nn-U2
  fold-to-false defect on `Object`-typed lhs noted in the code.

The fix presumably mirrors the fat-aware `iface == null` fix (2026-08-03):
extract the object half of the fat pair before the RTTI call, and include
interface-typed lhs in `wantRuntime`.

## 3. Impact and workaround

- Blocks runtime type dispatch on any interface-typed value — e.g.
  `Metric`-driven algorithm specialization.
- **Workaround (live in `cajeta.math.distance.Distance`):** static
  **overloads** dispatch on the *static* type instead
  (`pdist(Tensor, Metric)` generic vs `pdist(Tensor, Euclidean)` expanded
  path). Overload resolution is verified correct: concrete-typed argument →
  specific overload; interface-typed argument → generic overload.

## 4. Acceptance

- **4.1** The §1 repro returns 1.
- **4.2** `instanceof` on an interface-typed lhs answers the runtime type
  truthfully for a matching class, a non-matching class, and a second
  interface.
- **4.3** The guarded form `if (m instanceof Euclidean e) { ... }` binds
  correctly from an interface-typed lhs.
- **4.4** `Distance.pdist`/`cdist` can drop the overload workaround (keep
  the overloads if they're judged the better API; the *requirement* is only
  that `instanceof` no longer crashes).
