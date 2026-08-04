#!/usr/bin/env python3
# gen_distributions.py — oracle fixtures for cajeta.math.stats discrete
# distributions (stdlib-completion plan Unit 2; spec §3, §9.3, §9.4).
# Oracle: scipy 1.18.0, PINNED. Regenerate as for gen_distance.py.

import scipy
from scipy import stats

assert scipy.__version__ == "1.18.0", f"oracle pin violated: {scipy.__version__}"

print("# hand-computable (§9.3)")
print("binom_pmf_2_6_05  =", repr(float(stats.binom.pmf(2, 6, 0.5))))    # 15/64
print("binom_pmf_7_10_08 =", repr(float(stats.binom.pmf(7, 10, 0.8))))

print("# large n (§9.4): naive factorial overflows, log-space must not")
print("binom_pmf_300_1000_03 =", repr(float(stats.binom.pmf(300, 1000, 0.3))))
print("binom_pmf_950_1000_095 =", repr(float(stats.binom.pmf(950, 1000, 0.95))))

print("# binomial CDF grid")
for (k, n, p) in [(2, 6, 0.5), (7, 10, 0.8), (0, 6, 0.1), (5, 6, 0.9),
                  (25, 50, 0.5), (300, 1000, 0.3), (1, 10, 0.1)]:
    print(f"binom_cdf_{k}_{n}_{str(p).replace('.', '')} =",
          repr(float(stats.binom.cdf(k, n, p))))

print("# bernoulli (n=1) exact")
print("bern_pmf_1_03 =", repr(float(stats.bernoulli.pmf(1, 0.3))))
print("bern_pmf_0_03 =", repr(float(stats.bernoulli.pmf(0, 0.3))))
print("bern_cdf_0_03 =", repr(float(stats.bernoulli.cdf(0, 0.3))))

print("# poisson pmf/cdf")
for (k, lam) in [(0, 0.5), (2, 0.5), (2, 3.0), (5, 3.0), (10, 10.0),
                 (0, 3.0), (20, 10.0)]:
    print(f"pois_pmf_{k}_{str(lam).replace('.', '')} =",
          repr(float(stats.poisson.pmf(k, lam))))
    print(f"pois_cdf_{k}_{str(lam).replace('.', '')} =",
          repr(float(stats.poisson.cdf(k, lam))))
