# cajeta.math.stats — distributions, hypothesis tests, information theory

New in the stdlib-completion batch (spec `stdlib-completion` §3–§5). All
of it is **additive** — the descriptive surface (`Stats.histogram`,
`cov`, `quantile`, the special functions) is unchanged. Parity oracle:
scipy 1.18.0, pinned in `tools/fixtures/gen_{distributions,hypothesis,
information}.py`.

## Discrete distributions — `Binomial`, `Bernoulli`, `Poisson`

PMF, CDF, and seeded sampling for each. Binomial's PMF evaluates in log
space through `Stats.gammaLn` (large `n` never overflows); its CDF is the
regularized incomplete beta. Poisson's CDF is the regularized upper
incomplete gamma. Invalid parameters (`p` outside `[0,1]`, negative `n`
or `lambda`) throw `StatsException` naming the parameter — never NaN.

```cajeta
float64 p = Binomial.pmf(2, 6, 0.5);            // 15/64
float64 c = Poisson.cdf(5, 3.0);
Generator g = heap Generator(42);
Tensor<int64> draws = Binomial.sample(6, 0.5, 10000, g);  // reproducible
```

## Hypothesis tests — `Hypothesis`, `TestResult`, `Alternative`

One-sample / two-sample / paired t-tests, chi-square goodness-of-fit and
independence, one-way ANOVA. Every result is a `TestResult` carrying
statistic, p-value, df (+`dfWithin` for F), the `Alternative` tested, and
an assumption-`warnings` bitmask (`WARN_SMALL_N`, `WARN_ZERO_VARIANCE`,
`WARN_LOW_EXPECTED`) — a violated assumption warns, never silently.

**The two-sample default is WELCH** (unequal variances — R's default;
scipy defaults to pooled). Pass `equalVar = true` for the pooled form.
Chi-square independence applies **no Yates correction**.

```cajeta
TestResult r = Hypothesis.tTest2(a, b, Alternative.TWO_SIDED);  // Welch
if (r.warnings != 0) { /* report, don't trust blindly */ }
```

## Information theory — `Information`

`klDivergence(p, q)` (= scipy `rel_entr` summed, in nats — ASYMMETRIC,
mind the argument order), `entropy(p, base)` and `crossEntropy(p, q,
base)` with the log base REQUIRED (trees want base 2, losses base e).
`q=0` where `p>0` yields +infinity — never NaN; `0·log 0 = 0`.
