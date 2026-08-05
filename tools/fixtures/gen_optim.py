#!/usr/bin/env python3
# gen_optim.py — oracle fixtures for cajeta.math.optim
# (stdlib-completion plan Unit 5; spec §8.6).
# Oracle: scipy 1.18.0, PINNED. Convergence parity is to the shared
# MINIMIZER (both libraries reach the same minimum within tolerance), not
# trajectory equality — line-search internals differ by design.

import numpy as np
import scipy
from scipy.optimize import minimize, rosen

assert scipy.__version__ == "1.18.0", f"oracle pin violated: {scipy.__version__}"

x0 = np.array([-1.2, 1.0])

r = minimize(rosen, x0, method="L-BFGS-B")
print("rosen lbfgsb:", r.x.tolist(), float(r.fun), r.nit)

r = minimize(rosen, x0, method="Nelder-Mead", options={"xatol": 1e-8, "fatol": 1e-10})
print("rosen nm:    ", r.x.tolist(), float(r.fun), r.nit)

def quad(x):
    return (x[0] - 3.0) ** 2 + 2.0 * (x[1] + 1.0) ** 2

r = minimize(quad, np.array([0.0, 0.0]), method="L-BFGS-B")
print("quad lbfgsb: ", r.x.tolist(), float(r.fun), r.nit)
r = minimize(quad, np.array([0.0, 0.0]), method="Nelder-Mead",
             options={"xatol": 1e-8, "fatol": 1e-10})
print("quad nm:     ", r.x.tolist(), float(r.fun), r.nit)

# AR(1) conditional-sum-of-squares smoke (5.3.3): fixed series, phi by OLS
rng = np.random.default_rng(42)
n = 30
phi_true = 0.6
e = rng.normal(0, 0.5, n)
x = np.zeros(n)
for t in range(1, n):
    x[t] = phi_true * x[t - 1] + e[t]
num = (x[1:] * x[:-1]).sum()
den = (x[:-1] ** 2).sum()
print("ar1 series =", [round(float(v), 6) for v in x])
print("ar1 phi_ols =", repr(float(num / den)))
