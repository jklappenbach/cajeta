# cajeta.math.distance + cajeta.math.optim — kernels and optimizers

Two new packages from the stdlib-completion batch. Parity oracle scipy
1.18.0 (`tools/fixtures/gen_distance.py`, `gen_optim.py`).

## `cajeta.math.distance` — the ONE distance implementation

Euclidean, manhattan, chebyshev, minkowski(p), cosine, and Pearson —
behind the `Metric` interface, so every consumer (k-NN, clustering,
similarity search) takes a `Metric` and hardcodes nothing. `pdist(x, m)`
returns the full `(n, n)` matrix with symmetry and a zero diagonal EXACT
by construction; `cdist(x, y, m)` is the cross form.

Passing a `Euclidean`-typed argument selects expanded-form matmul
overloads of `pdist`/`cdist` (`‖a‖² + ‖b‖² − 2a·b`, one GEMM) — with the
documented caveat that near-zero distances between large-norm points lose
precision to cancellation; the direct `Distance.euclidean` is accurate
there. Zero-norm cosine is DEFINED (one zero operand → similarity 0;
both → 1), deliberately diverging from scipy's NaN.

Ecosystem rule (spec §9.1): no second distance implementation anywhere —
libraries consume this one.

```cajeta
Tensor<float64> m #= Distance.pdist(points, heap Euclidean());  // matmul path
Metric custom = heap MyMetric();                               // your own
Tensor<float64> c #= Distance.cdist(a, b, custom);
```

## `cajeta.math.optim` — L-BFGS and Nelder-Mead

`Lbfgs.minimize(objective, x0, maxIter)` — strong-Wolfe line search;
numerical central-difference gradients by default, or implement
`GradObjective` for analytic ones (the overload dispatches on the static
type). `NelderMead.minimize(...)` — the gradient-free fallback for
likelihoods without clean derivatives (ARMA/ARIMA CSS is the motivating
consumer).

Results carry the minimizer, value, iterations, and an explicit
`TerminationReason`; an iteration cap or non-finite objective reports
`converged = false` — never disguised as success.
