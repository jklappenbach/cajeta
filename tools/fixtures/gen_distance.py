#!/usr/bin/env python3
# gen_distance.py — oracle fixtures for cajeta.math.distance
# (stdlib-completion plan Unit 1; spec §2, §9.1, §9.2).
#
# Oracle: scipy 1.18.0, PINNED — the version is asserted, not assumed.
# Regenerate with:
#   python3 -m venv /tmp/scipy118 && /tmp/scipy118/bin/pip install scipy==1.18.0
#   /tmp/scipy118/bin/python tools/fixtures/gen_distance.py
# Output is the constants block pasted into test/math/DistanceTests.cpp.

import numpy as np
import scipy
from scipy.spatial import distance as sd

assert scipy.__version__ == "1.18.0", f"oracle pin violated: {scipy.__version__}"

np.set_printoptions(precision=17)


def emit(name, val):
    print(f"{name} = {val!r}")


# --- 1.1.1 hand cases (verifiable on paper; scipy only confirms) -------------
a2, b2 = np.array([0.0, 0.0]), np.array([3.0, 4.0])
a3, b3 = np.array([1.0, 2.0, 3.0]), np.array([4.0, 6.0, 3.0])
emit("euclid2", sd.euclidean(a2, b2))          # 5
emit("cityblock2", sd.cityblock(a2, b2))       # 7
emit("chebyshev2", sd.chebyshev(a2, b2))       # 4
emit("minkowski2_p3", sd.minkowski(a2, b2, 3))  # 91^(1/3)
emit("euclid3", sd.euclidean(a3, b3))          # 5
emit("cityblock3", sd.cityblock(a3, b3))       # 7
emit("chebyshev3", sd.chebyshev(a3, b3))       # 4
emit("minkowski3_p3", sd.minkowski(a3, b3, 3))
emit("cosine_orth", sd.cosine([1.0, 0.0], [0.0, 1.0]))       # 1
emit("cosine_par", sd.cosine([1.0, 2.0, 2.0], [2.0, 4.0, 4.0]))  # 0
emit("cosine_gen3", sd.cosine(a3, b3))

# --- 1.1.6 zero-norm cosine: record scipy 1.18's convention ------------------
z = np.zeros(3)
with np.errstate(invalid="ignore"):
    emit("cosine_zero_vs_b3", float(sd.cosine(z, b3)))
    emit("cosine_zero_zero", float(sd.cosine(z, z)))

# --- 1.1.7 Pearson correlation ----------------------------------------------
u = np.array([1.0, 2.0, 3.0, 5.0, 8.0])
v = np.array([0.11, 0.12, 0.13, 0.15, 0.18])
emit("pearson_r_uv", float(np.corrcoef(u, v)[0, 1]))     # exactly 1 by design
u2 = np.array([2.0, 1.0, 4.0, 3.0, 7.0, 5.0])
v2 = np.array([1.9, 2.2, 3.5, 4.1, 5.6, 7.3])
emit("pearson_r_u2v2", float(np.corrcoef(u2, v2)[0, 1]))
emit("correlation_dist_u2v2", float(sd.correlation(u2, v2)))  # 1 - r

# --- 1.3.3 pdist/cdist parity fixture (4 points in R^3, fixed values) --------
X = np.array([
    [0.2, -1.3, 2.7],
    [1.9, 0.4, -0.6],
    [-2.1, 3.3, 1.1],
    [0.7, 0.9, -1.8],
])
Y = X[:2] + 0.5  # 2x3 for cdist
for metric, tag in [("euclidean", "eu"), ("cityblock", "cb"),
                    ("chebyshev", "ch"), ("cosine", "cos"),
                    ("correlation", "corr")]:
    emit(f"pdist_{tag}", list(sd.pdist(X, metric)))
    emit(f"cdist_{tag}", [list(r) for r in sd.cdist(X, Y, metric)])
emit("pdist_mink_p3", list(sd.pdist(X, "minkowski", p=3.0)))
emit("cdist_mink_p3", [list(r) for r in sd.cdist(X, Y, "minkowski", p=3.0)])
