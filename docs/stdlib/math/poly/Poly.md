# Poly

`cajeta.math.poly.Poly` — polynomial operations. Coefficients are a 1-D
`Tensor<E>` in numpy order, highest degree first: `[a_n, …, a_1, a_0]`
represents `a_n·x^n + … + a_0`. `polyval` evaluates by Horner's method,
`polyadd` aligns coefficients by degree, `polymul` convolves the coefficient
sequences. Root-finding and fitting build on the linalg tier and are
deferred.

```cajeta
package snip.poly;

import cajeta.math.Tensor;
import cajeta.math.poly.Poly;

public final class Demo {
    public static void run() {
        int64[] shp = heap int64[1];
        shp[0] = 3;
        Tensor<float32> c = Tensor.zeros<float32>(shp);
        c.set1(0, 1.0f);                                // x^2 + 2x + 3
        c.set1(1, 2.0f);
        c.set1(2, 3.0f);
        float32 y = Poly.polyval<float32>(c, 2.0f);     // 11
        Tensor<float32> sq = Poly.polymul<float32>(c, c);
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static E polyval<E extends Numeric>(Tensor<E> coeffs, E x)` ⚑ | Evaluate the polynomial `coeffs` (highest degree first) at `x` via Horner's method (numpy `polyval`) |
| `static #Tensor<E> polyadd<E extends Numeric>(Tensor<E> a, Tensor<E> b)` | Sum of two polynomials (numpy `polyadd`): coefficients aligned by degree, the shorter padded on the high-degree end |
| `static #Tensor<E> polymul<E extends Numeric>(Tensor<E> a, Tensor<E> b)` ⚑ | Product of two polynomials (numpy `polymul`): the convolution `out[i+j] += a[i]·b[j]` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/poly/Poly.cajeta`](../../../../runtime/src/cajeta/math/poly/Poly.cajeta)
- [Tensor](../Tensor.md) — the coefficient carrier, [LinAlg](../linalg/LinAlg.md) — the tier root-finding will build on
