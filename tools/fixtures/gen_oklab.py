#!/usr/bin/env python3
# gen_oklab.py — reference fixtures for OKLab on cajeta.math.Color
# (stdlib-completion plan Unit 6; spec §6, §9.7).
# Reference implementation: Björn Ottosson's published OKLab matrices
# (https://bottosson.github.io/posts/oklab/), evaluated in float64.
# No scipy involvement — the oracle here is the published constants.

import numpy as np

M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
               [0.2119034982, 0.6806995451, 0.1073969566],
               [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
               [1.9779984951, -2.4285922050, 0.4505937099],
               [0.0259040371, 0.7827717662, -0.8086757660]])

def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def linear_to_srgb(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.cbrt(c ** (1.25 / 3.0)) ** 0)  # unused

def to_oklab(rgb):
    lms = M1 @ srgb_to_linear(rgb)
    return M2 @ np.cbrt(lms)

def from_oklab(lab):
    lms_ = np.linalg.inv(M2) @ np.asarray(lab, dtype=np.float64)
    lin = np.linalg.inv(M1) @ (lms_ ** 3)
    return lin  # linear rgb, possibly out of [0,1]

for name, rgb in [("white", [1, 1, 1]), ("black", [0, 0, 0]),
                  ("red", [1, 0, 0]), ("green", [0, 1, 0]), ("blue", [0, 0, 1])]:
    print(name, "=", [repr(float(v)) for v in to_oklab(rgb)])

red = to_oklab([1, 0, 0])
blue = to_oklab([0, 0, 1])
print("deltaE_red_blue =", repr(float(np.linalg.norm(red - blue))))

# blue hue in OKLCh (degrees, wrapped to [0, 360))
h = np.degrees(np.arctan2(blue[2], blue[1])) % 360.0
c = float(np.hypot(blue[1], blue[2]))
print("blue_lch = (L, C, h):", repr(float(blue[0])), repr(c), repr(float(h)))

# out-of-gamut probe: L=0.7, C=0.3, h=30deg — linear rgb outside [0,1]?
hh = np.radians(30.0)
lab = [0.7, 0.3 * np.cos(hh), 0.3 * np.sin(hh)]
lin = from_oklab(lab)
print("oog_linear_rgb =", [repr(float(v)) for v in lin], "outside:", bool((lin < 0).any() or (lin > 1).any()))
