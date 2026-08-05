#!/usr/bin/env python3
# gen_information.py — oracle fixtures for cajeta.math.stats information
# theory (stdlib-completion plan Unit 4; spec §5, §9.6).
# Oracle: scipy 1.18.0, PINNED. KL matches rel_entr SUMMED (natural log).

import numpy as np
import scipy
from scipy.special import rel_entr

assert scipy.__version__ == "1.18.0", f"oracle pin violated: {scipy.__version__}"

p = np.array([0.5, 0.3, 0.2])
q = np.array([0.2, 0.5, 0.3])
print("kl_pq =", repr(float(rel_entr(p, q).sum())))
print("kl_qp =", repr(float(rel_entr(q, p).sum())))

pz = np.array([0.0, 0.6, 0.4])          # p has a zero: term contributes 0
qz = np.array([0.5, 0.25, 0.25])
print("kl_pz_qz =", repr(float(rel_entr(pz, qz).sum())))

print("# entropy: hand base-2 (ml-trees-ensembles §3.5) and scipy base-e")
h = np.array([0.625, 0.375])
ln = -(h * np.log(h)).sum()
print("H2_625_375 =", repr(float(ln / np.log(2.0))))   # ~0.9544
print("He_625_375 =", repr(float(ln)))
he_p = -(p * np.log(p)).sum()
print("He_p =", repr(float(he_p)))
print("H2_p =", repr(float(he_p / np.log(2.0))))

print("# cross-entropy -sum p log q, both bases")
ce = -(p * np.log(q)).sum()
print("CEe_pq =", repr(float(ce)))
print("CE2_pq =", repr(float(ce / np.log(2.0))))
print("identity: CEe_pq == He_p + kl_pq ->", np.isclose(ce, he_p + rel_entr(p, q).sum()))
