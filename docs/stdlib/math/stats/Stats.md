# Stats

`cajeta.math.stats.Stats` — the descriptive-statistics surface: binning
(`histogram`/`bincount`/`digitize`), covariance and correlation
(`cov`/`corrcoef`), and the order statistics
(`quantile`/`percentile`/`median`). All static, over `Tensor` inputs, on the
CPU floor.

```cajeta
package snip.stats;

import cajeta.math.Tensor;
import cajeta.math.stats.Stats;

public final class Demo {
    public static void run() {
        Tensor<float32> t = Tensor.arange<float32>(100);
        Tensor<int64> counts = Stats.histogram<float32>(t, 10, 0.0f, 100.0f);
        float32 med = Stats.median<float32>(t);
        float32 p90 = Stats.percentile<float32>(t, 90.0f);
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static #Tensor<int64> histogram<E extends Numeric>(Tensor<E> t, int32 bins, E lo, E hi)` ⚑ | Histogram of `t` over `bins` equal-width buckets spanning `[lo, hi]` (numpy `histogram` counts): a `Tensor<int64>` of length `bins` |
| `static #Tensor<int64> bincount(Tensor<int64> t)` | Count of each non-negative integer value in the 1-D `t` (numpy `bincount`): length `max(t)+1`, `out[v]` = occurrences of `v` |
| `static #Tensor<int64> digitize<E extends Numeric>(Tensor<E> t, Tensor<E> edges)` | Bin index of each element of the 1-D `t` relative to the increasing `edges` (numpy `digitize`, `right=False`): `out[i]` is the count of edges `<= t[i]` |
| `static #Tensor<E> cov<E extends Floating>(Tensor<E> m)` | Covariance matrix of `m` shaped `(variables, observations)` (numpy `cov`, `rowvar=True`), the unbiased `ddof=1` estimator |
| `static #Tensor<E> corrcoef<E extends Floating>(Tensor<E> m)` | Pearson correlation matrix: covariance normalized by the per-variable standard deviations — diagonal 1, entries in `[-1, 1]` |
| `static E quantile<E extends Floating>(Tensor<E> t, E q)` ⚑ | The `q`-th quantile of the flattened input (`q` in `[0,1]`, numpy `quantile`): sorts, then linearly interpolates at `q·(n-1)` |
| `static E percentile<E extends Floating>(Tensor<E> t, E p)` | The `p`-th percentile (`p` in `[0,100]`) — `quantile(t, p/100)` |
| `static E median<E extends Floating>(Tensor<E> t)` | The median of the flattened input — `quantile(t, 0.5)` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/stats/Stats.cajeta`](../../../../runtime/src/cajeta/math/stats/Stats.cajeta)
- [Tensor](../Tensor.md) — whole-tensor reductions (`mean`, `variance`, `std`) live on the core type, [Generator](../random/Generator.md) — sampling
