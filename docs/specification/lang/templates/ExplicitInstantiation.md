# Explicit instantiation: `@Instantiate` and `@Requires`

A class template is built once for each instantiation that code names. A
library sometimes needs instantiations that no code names yet. A tile family
is one example: it ships several shapes so that a measured choice can pick
one at run time. `@Instantiate` lists those instantiations, and each listed
one is built with the rest of the program.

```cajeta
@Instantiate({Tile<128, 128, 32, 64>, Tile<128, 128, 32, 32>, Tile<64, 64, 32, 32>})
@Requires(WM % 16 == 0 && WN % 16 == 0)
public class Tile<uint32 TM, uint32 TN, uint32 WM, uint32 WN> {
    @Kernel
    @Occupancy(maxWaves = (TM / WM) * (TN / WN))
    public static void run(KernelBuffer<float32> c) { ... }
}
```

## `@Instantiate`

- Each entry is an instantiation of the class that carries the annotation,
  written as a type: `Tile<4>`, or with its package, `test.Tile<4>`. An entry
  that names another class is refused.
- Every listed instantiation is built at compile time, including its
  kernels, so an ahead-of-time executable carries all of them. An entry that
  code also names is built once.
- The list has a cap, so its compile cost is known up front. The cap is 8
  unless the class declares another with `max`:
  `@Instantiate(value = {Tile<4>, Tile<7>}, max = 12)`. A longer list is
  refused, naming the class, the count and the cap.

## `@Requires`

`@Requires(<expression>)` states which instantiations the template's body
can serve. The expression is an integer constant expression over the
template's values. It can use literals, the non-type parameters, arithmetic,
shifts, bitwise operators, comparisons, `!`, `&&` and `||`.

The condition is checked at every instantiation, listed or named in code.
One that is false is refused before anything is built, with a message that
names the instantiation and quotes the condition:

```
Tile<24> is refused: test.Tile requires N % 16 == 0, which is false for Tile<24>
```

A condition that is not a constant expression, such as one naming a field,
is refused too, naming what could not be bound.

## See also

- `docs/specification/xpu/CajetaXPU.md` §14.4, for template values in
  `@Occupancy` and fragment-array sizes.
- `specs/xpu-kernel-independence-spec.md` §3.4 and §7.3.
