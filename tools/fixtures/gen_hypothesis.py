#!/usr/bin/env python3
# gen_hypothesis.py — oracle fixtures for cajeta.math.stats hypothesis tests
# (stdlib-completion plan Unit 3; spec §4, §9.5).
# Oracle: scipy 1.18.0, PINNED. Regenerate as for gen_distance.py.
# chi2_contingency is pinned with correction=False (cajeta applies no Yates
# correction; documented on Hypothesis.chiSquareIndependence).

import numpy as np
import scipy
from scipy import stats

assert scipy.__version__ == "1.18.0", f"oracle pin violated: {scipy.__version__}"

x = np.array([2.1, 2.5, 1.9, 2.8, 2.2, 2.6, 2.4, 2.0])
y = np.array([1.6, 1.8, 2.3, 1.5, 1.9, 2.1])          # shorter, other variance
d2 = np.array([2.3, 2.1, 2.2, 2.0, 2.5, 2.4, 1.8, 2.6])  # same length as x

print("# one-sample vs mu=2.0, all alternatives")
for alt in ["two-sided", "less", "greater"]:
    r = stats.ttest_1samp(x, 2.0, alternative=alt)
    print(f"t1_{alt.replace('-','')} = ({r.statistic!r}, {r.pvalue!r}, {r.df!r})")

print("# two-sample x vs y, both variance assumptions, two-sided")
r_eq = stats.ttest_ind(x, y, equal_var=True)
r_w = stats.ttest_ind(x, y, equal_var=False)
print(f"t2_equal = ({r_eq.statistic!r}, {r_eq.pvalue!r}, {r_eq.df!r})")
print(f"t2_welch = ({r_w.statistic!r}, {r_w.pvalue!r}, {r_w.df!r})")

print("# two-sample one-sided (welch, greater)")
r_wg = stats.ttest_ind(x, y, equal_var=False, alternative="greater")
print(f"t2_welch_greater = ({r_wg.statistic!r}, {r_wg.pvalue!r})")

print("# paired x vs d2 (and the UNpaired contrast on the same data)")
r_rel = stats.ttest_rel(x, d2)
r_ind = stats.ttest_ind(x, d2, equal_var=False)
print(f"t_paired = ({r_rel.statistic!r}, {r_rel.pvalue!r}, {r_rel.df!r})")
print(f"t_unpaired_same_data = ({r_ind.statistic!r}, {r_ind.pvalue!r})")

print("# chi-square goodness of fit")
obs = np.array([18.0, 22.0, 27.0, 13.0, 20.0])
exp = np.array([20.0, 20.0, 20.0, 20.0, 20.0])
r = stats.chisquare(obs, exp)
print(f"chi_gof = ({r.statistic!r}, {r.pvalue!r})   # df=4")

print("# chi-square independence, correction=False")
table = np.array([[12.0, 5.0, 9.0], [8.0, 14.0, 6.0]])
r = stats.chi2_contingency(table, correction=False)
print(f"chi_ind = ({r.statistic!r}, {r.pvalue!r}, {r.dof!r})")

print("# one-way ANOVA, three groups")
g1 = np.array([4.1, 3.9, 4.3, 4.0, 4.2])
g2 = np.array([3.5, 3.8, 3.6, 3.9, 3.4])
g3 = np.array([4.4, 4.6, 4.2, 4.7, 4.5])
r = stats.f_oneway(g1, g2, g3)
print(f"anova = ({r.statistic!r}, {r.pvalue!r})   # df=(2,12)")
