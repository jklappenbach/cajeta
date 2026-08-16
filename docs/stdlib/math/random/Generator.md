# Generator

`cajeta.math.random.Generator` — the random-number surface, built on a
counter-based BitGenerator (Philox4x32-10). Counter-based means each output
is a stateless function `philox(counter, key)` of its index and the seed: the
stream is reproducible, and element `i` can be computed independently of all
others — embarrassingly parallel, identical on CPU and GPU (the reason for
choosing Philox over a sequential generator like PCG64). A `Generator` holds
the 64-bit key (the seed).

```cajeta
package snip.rng;

import cajeta.math.Tensor;
import cajeta.math.random.Generator;

public final class Demo {
    public static void run() {
        Generator g = heap Generator(42);
        Tensor<float32> u #= g.uniform(1000);       // [0, 1)
        Tensor<float32> z #= g.normal(1000);        // N(0, 1)
        Tensor<int64> d #= g.integers(1, 7, 10);    // ten rolls of a die
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `Generator(uint64 seed)` ⚑ | Build a generator keyed by `seed`; the same seed reproduces the same stream |
| `#Tensor<float32> uniform(int64 n)` ⚑ | `n` samples from the uniform distribution over `[0, 1)`: element `i` is the top 24 bits of `philox(i, key)[0]` scaled to `[0, 1)` |
| `#Tensor<float32> normal(int64 n)` | `n` samples from the standard normal `N(0, 1)` via the Box-Muller transform over counters `2i` and `2i+1` |
| `#Tensor<int64> integers(int64 low, int64 high, int64 n)` | `n` uniform integers in `[low, high)`: element `i` is `low + (philox(i) mod (high-low))` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/random/Generator.cajeta`](../../../../runtime/src/cajeta/math/random/Generator.cajeta)
- [Tensor](../Tensor.md) — the sample carrier, [Stats](../stats/Stats.md) — descriptive statistics over the samples
