#!/usr/bin/env python3
# math/ numeric competitor, Python: NumPy and SciPy (benchmark-reference unit 11,
# spec §6.8). Implements the shared workloads (sizes, data, checks) the Cajeta
# side runs, one v1 row (31 columns) per id/variant; run.sh maps them to v2.
# Only the workload is timed; data, references and checks are outside the timed
# region, and every check reads the last trial's output, so nothing elides.
# Env: PROFILE_RUN_ID, PROFILE_RUN_TS, PROFILE_WARMUP, PROFILE_TRIALS,
#      PROFILE_LANG_VERSION, MATH_IDS (space list of slugs), CAJETA_BENCH_SMOKE=1.
import io
import math
import os
import resource
import sys
import time
import traceback

SMOKE = os.environ.get("CAJETA_BENCH_SMOKE", "") in ("1", "true", "yes")


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
WARMUP = int(env("PROFILE_WARMUP", "1" if SMOKE else "3"))
TRIALS = int(env("PROFILE_TRIALS", "3" if SMOKE else "10"))
PYVER = env("PROFILE_LANG_VERSION", "")
IDS = env("MATH_IDS", "").split()


def wanted(slug):
    return not IDS or slug in IDS


def peak_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ")


def row(slug, size, lib, ver, warmup, trials, st, status, ok, notes, variant=""):
    mn, med, mean, p95 = st
    okc = "" if ok is None else str(ok).lower()
    return (f"1,{RUN_ID},{TS},{slug},math,,{size},,{size},python,{PYVER},{lib},{ver},CPython as shipped,"
            f"{warmup},{trials},{mn},{med},{mean},{p95},,,{peak_rss_kb()},"
            f"-1,-1,-1,-1,{status},{okc},{clean(notes)},{variant}")


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def timed(f):
    for _ in range(WARMUP):
        f()
    samples = []
    r = None
    for _ in range(TRIALS):
        t0 = time.perf_counter_ns()
        r = f()
        samples.append(time.perf_counter_ns() - t0)
    return stats(samples), r


import numpy as np  # noqa: E402

NPV = np.__version__
f32 = np.float32
P = 10007


def u(i):
    i = np.asarray(i, dtype=np.int64)
    return ((i * 7919) % P).astype(np.float64) / 10007.0


def v(i):
    i = np.asarray(i, dtype=np.int64)
    m = i % P
    return ((m * m * 7 + i * 7919) % P).astype(np.float64) / 10007.0


def smoke_note(what):
    return f"smoke size {what}; " if SMOKE else ""


def emit(slug, size, lib, ver, st, ok, notes, variant=""):
    print(row(slug, size, lib, ver, WARMUP, TRIALS, st,
              "ok" if ok else "invalid", bool(ok), notes, variant), flush=True)


# ---------------- Tensor ----------------

def b_element_get():
    n = 1000000
    a = np.arange(n, dtype=np.float64) * 0.5

    def f():
        s = 0.0
        for i in range(n):
            s += float(np.float32(a[i]))
        return s
    st, s = timed(f)
    emit("tensor-element-get", n, "numpy", NPV, st, s == 249999750000.0,
         "interpreted per-element a[i] indexing with float32 narrowing; check: sum == 249999750000 exactly", "flat-get")


def b_element_set():
    n = 1000000
    w = np.zeros(n, dtype=np.float64)
    t = [0.0]

    def f():
        t[0] += 1.0
        tv = t[0]
        for i in range(n):
            w[i] = tv + i
        return tv
    st, tv = timed(f)
    emit("tensor-element-set", n, "numpy", NPV, st, w[n - 1] == tv + (n - 1),
         "interpreted per-element w[i] = v stores; check: last element exact")


def b_elementwise():
    n = 1000000 if SMOKE else 10000000
    i = np.arange(n)
    a = u(i).astype(f32); b = u(i + 1).astype(f32); c = u(i + 2).astype(f32)
    ref = float((a.astype(np.float64) * b.astype(np.float64) + c.astype(np.float64)).sum())
    st, r = timed(lambda: a * b + c)
    s = float(r.astype(np.float64).sum())
    emit("tensor-elementwise", n, "numpy", NPV, st, abs(s - ref) <= 1e-5 * abs(ref),
         smoke_note("n=1e6 in place of 1e7") + "float32 a*b+c fresh result; check: rel err <= 1e-5")


def mat_m(rows, cols=1000):
    return u(np.arange(rows * cols)).astype(f32).reshape(rows, cols)


def b_broadcast():
    rows = 1000 if SMOKE else 10000
    M = mat_m(rows)
    rv = u(np.arange(1000) + 5).astype(f32)
    ref = float((M.astype(np.float64) + rv.astype(np.float64)[None, :]).sum())
    st, R = timed(lambda: M + rv)
    s = float(R.astype(np.float64).sum())
    emit("tensor-broadcast", rows * 1000, "numpy", NPV, st, abs(s - ref) <= 1e-5 * abs(ref),
         smoke_note("rows=1000 in place of 10000") + "float32 row broadcast; check: rel err <= 1e-5")


def b_reduce():
    rows = 1000 if SMOKE else 10000
    M = mat_m(rows)
    D = M.astype(np.float64)
    ref = (D.sum(0).sum() + D.sum(1).sum() + D.max(0).sum() + D.max(1).sum()
           + D.mean(0).sum() + D.mean(1).sum())

    def f():
        return (M.sum(0), M.sum(1), M.max(0), M.max(1), M.mean(0), M.mean(1))
    st, r = timed(f)
    cs = sum(float(x.astype(np.float64).sum()) for x in r)
    emit("tensor-reduce", rows * 1000, "numpy", NPV, st, abs(cs - ref) <= 1e-3 * abs(ref),
         smoke_note("rows=1000 in place of 10000") + "float32 sum/max/mean along both axes; check: rel err <= 1e-3")


def b_transpose():
    N = 1024 if SMOKE else 4096
    M = u(np.arange(N * N)).astype(f32).reshape(N, N)
    st, T = timed(lambda: np.ascontiguousarray(M.T))
    ok = T.flags["C_CONTIGUOUS"] and np.array_equal(T, M.T)
    emit("tensor-transpose", N * N, "numpy", NPV, st, ok,
         smoke_note("N=1024 in place of 4096") + "ascontiguousarray(M.T); check: exact equality")


def b_cast():
    n = 1000000 if SMOKE else 10000000
    x = (u(np.arange(n)) * 1000 - 500).astype(f32)
    ref = int(np.trunc(x.astype(np.float64)).astype(np.int64).sum())

    def f():
        y = x.astype(np.float64)
        return y, y.astype(np.int32)
    st, (y, z) = timed(f)
    emit("tensor-cast", n, "numpy", NPV, st, int(z.astype(np.int64).sum()) == ref,
         smoke_note("n=1e6 in place of 1e7") + "astype float64 then int32 (truncating); check: int64 sum exact")


# ---------------- LinAlg ----------------

def lin_a(n):
    A = u(np.arange(n * n)).reshape(n, n) - 0.5
    A[np.diag_indices(n)] += n
    b = u(np.arange(n) + 3)
    return A, b


LIN_SMOKE = {"linalg-solve": (128, 256), "linalg-inverse": (128, 256),
             "linalg-qr": (128, 256), "linalg-svd": (64, 128)}


def b_linalg(slug):
    import scipy
    import scipy.linalg as sl
    sv = scipy.__version__
    for k, variant in enumerate(("512", "2048")):
        n = LIN_SMOKE[slug][k] if SMOKE else int(variant)
        sn = smoke_note(f"n={n} in place of {variant}")
        A, b = lin_a(n)
        if slug == "linalg-solve":
            st, x = timed(lambda: sl.solve(A, b))
            r = np.abs(A @ x - b).max() / (np.abs(A).sum(1).max() * np.abs(x).max() + np.abs(b).max())
            ok, note = r <= 1e-12, "scipy.linalg.solve (LAPACK gesv); check: scaled residual inf-norm <= 1e-12"
        elif slug == "linalg-inverse":
            st, Ai = timed(lambda: sl.inv(A))
            r = np.abs(A @ Ai - np.eye(n)).max()
            ok, note = r <= 1e-10, "scipy.linalg.inv (LAPACK getrf+getri); check: max|A*Ai-I| <= 1e-10"
        elif slug == "linalg-qr":
            st, (Q, R) = timed(lambda: sl.qr(A))
            r = np.abs(Q @ R - A).max() / np.abs(A).max()
            ok, note = r <= 1e-12, "scipy.linalg.qr (LAPACK geqrf+orgqr); check: max|QR-A|/max|A| <= 1e-12"
        else:
            st, (U, S, Vt) = timed(lambda: sl.svd(A, full_matrices=True))
            r = np.abs((U * S) @ Vt - A).max() / np.abs(A).max()
            ok, note = r <= 1e-10, "scipy.linalg.svd full (LAPACK gesdd); check: max|U diag(S) Vt-A|/max|A| <= 1e-10"
        emit(slug, n, "scipy", sv, st, ok, sn + note + f"; measured {r:.2e}", variant)


# ---------------- Fft ----------------

def sig(N):
    k = np.arange(N)
    return (u(2 * k) - 0.5).astype(f32) + 1j * (u(2 * k + 1) - 0.5).astype(f32)


def b_fft1d():
    x64 = sig(64)
    j = np.arange(64)
    W = np.exp(-2j * np.pi * np.outer(j, j) / 64)
    e_small = np.abs(np.fft.fft(x64) - W @ x64).max()
    for variant, full, smk in (("2^16", 1 << 16, 1 << 16), ("2^20", 1 << 20, 1 << 18)):
        N = smk if SMOKE else full
        x = sig(N).astype(np.complex128)
        st, X = timed(lambda: np.fft.fft(x))
        e_rt = np.abs(np.fft.ifft(X) - x).max()
        sn = smoke_note(f"N={N} in place of {full}") if N != full else ""
        emit("fft-1d", N, "numpy", NPV, st, e_small <= 1e-4 and e_rt <= 1e-4,
             sn + "numpy.fft is pocketfft in complex128 (no complex64 path); check: direct DFT N=64 and round trip max abs err <= 1e-4"
             + f"; measured {e_small:.1e}/{e_rt:.1e}", variant)


def b_fft2d():
    n = 512 if SMOKE else 2048
    x = sig(n * n).astype(np.complex128).reshape(n, n)
    st, X = timed(lambda: np.fft.fft2(x))
    e = np.abs(np.fft.ifft2(X) - x).max()
    emit("fft-2d", n * n, "numpy", NPV, st, e <= 1e-4,
         smoke_note("512x512 in place of 2048x2048") + "numpy.fft.fft2 (pocketfft complex128); check: round trip max abs err <= 1e-4" + f"; measured {e:.1e}")


# ---------------- Random ----------------

def b_random(slug):
    n = 10000000 if SMOKE else 100000000
    rng = np.random.default_rng(42)
    buf = np.empty(n, dtype=np.float64)
    if slug == "random-uniform":
        st, _ = timed(lambda: rng.random(out=buf))
        m, va = float(buf.mean()), float(buf.var())
        ok = abs(m - 0.5) <= 1e-3 and abs(va - 1 / 12) <= 1e-3
        note = "default_rng(42) PCG64 random(out=); check: |mean-0.5| <= 1e-3 and |var-1/12| <= 1e-3"
    else:
        st, _ = timed(lambda: rng.standard_normal(out=buf))
        m, va = float(buf.mean()), float(buf.var())
        ok = abs(m) <= 1e-3 and abs(va - 1) <= 2e-3
        note = "default_rng(42) PCG64 standard_normal(out=) ziggurat; check: |mean| <= 1e-3 and |var-1| <= 2e-3"
    emit(slug, n, "numpy", NPV, st, ok, smoke_note("n=1e7 in place of 1e8") + note)


# ---------------- Npy ----------------

NPY_N = 1 << 23


def npy_tensor():
    t = np.zeros(NPY_N, dtype="<i4")
    t[0] = 7
    t[-1] = 8247
    return t


def npy_reference(t):
    h = b"{'descr': '<i4', 'fortran_order': False, 'shape': (8388608,), }"
    h = h + b" " * (128 - 10 - len(h) - 1) + b"\n"
    return b"\x93NUMPY\x01\x00" + len(h).to_bytes(2, "little") + h + t.tobytes()


def b_npy_load():
    t = npy_tensor()
    img = npy_reference(t)
    st, a = timed(lambda: np.load(io.BytesIO(img)))
    ok = len(a) == NPY_N and int(a.astype(np.int64).sum()) == 8254 and a[0] == 7 and a[-1] == 8247
    emit("npy-load-i32", len(img), "numpy", NPV, st, ok, "np.load from BytesIO; check: len and sum 8254 and ends")


def b_npy_save():
    t = npy_tensor()
    ref = npy_reference(t)

    def f():
        b = io.BytesIO()
        np.save(b, t)
        return b.getvalue()
    st, img = timed(f)
    emit("npy-save-i32", len(ref), "numpy", NPV, st, img == ref, "np.save to BytesIO; check: byte equality with the reference image")


# ---------------- Poly ----------------

COEF = np.array([0.5, -1.25, 2.0, -0.75, 1.5, -2.25, 0.25, 1.75, -1.0])


def b_poly_eval():
    n = 10000000 if SMOKE else 100000000
    x = np.arange(n, dtype=np.float64) / n
    integ = sum(COEF[k] / (9 - k) for k in range(9))
    ref = n * integ - (np.polyval(COEF, 1.0) - np.polyval(COEF, 0.0)) / 2
    st, y = timed(lambda: np.polyval(COEF, x))
    s = float(y.sum())
    emit("poly-eval", n, "numpy", NPV, st, abs(s - ref) <= 1e-7 * abs(ref),
         smoke_note("n=1e7 in place of 1e8") + "np.polyval (Horner over arrays); check: rel err <= 1e-7 vs Euler-Maclaurin reference")


def b_poly_fit():
    m, fits = (100000, 10) if SMOKE else (1000000, 100)
    x = -1 + 2 * np.arange(m, dtype=np.float64) / (m - 1)
    y = np.polyval(COEF, x)

    def f():
        c = None
        for _ in range(fits):
            c = np.polyfit(x, y, 8)
        return c
    st, c = timed(f)
    e = np.abs(c - COEF).max()
    emit("poly-fit", m * fits, "numpy", NPV, st, e <= 1e-6,
         smoke_note("m=1e5 and 10 fits in place of 1e6 and 100") + f"np.polyfit deg 8 (lstsq); {fits} fits per trial; check: max coef err <= 1e-6; measured {e:.1e}")


# ---------------- Stats ----------------

def stats_data():
    n = 10000000 if SMOKE else 100000000
    return n, u(np.arange(n))


def b_moments():
    import scipy
    import scipy.stats as ss
    n, x = stats_data()

    def f():
        return x.mean(), x.var(), ss.skew(x), ss.kurtosis(x)
    st, (m, va, sk, ku) = timed(f)
    ok = abs(m - 0.49995) <= 1e-3 and abs(va - 1 / 12) <= 1e-3 and abs(sk) <= 1e-3 and abs(ku + 1.2) <= 1e-3
    emit("stats-moments", n, "scipy", scipy.__version__, st, ok,
         smoke_note("n=1e7 in place of 1e8") + "numpy mean/var + scipy.stats skew/kurtosis (biased Fisher); check: each moment abs err <= 1e-3")


def b_histogram():
    n, x = stats_data()
    k = (np.arange(n, dtype=np.int64) * 7919) % P
    ref = np.bincount(k * 1000 // P, minlength=1000)
    st, (cnt, _) = timed(lambda: np.histogram(x, bins=1000, range=(0.0, 1.0)))
    emit("stats-histogram", n, "numpy", NPV, st, np.array_equal(cnt, ref),
         smoke_note("n=1e7 in place of 1e8") + "np.histogram 1000 bins over [0 1]; check: every count exact")


# ---------------- Geometry ----------------

def b_transform():
    m = 1000000
    passes = 10 if SMOKE else 100
    k = np.arange(m)
    p = np.stack([v(3 * k) * 10 - 5, v(3 * k + 1) * 10 - 5, v(3 * k + 2) * 10 - 5], 1).astype(f32)
    ax = np.array([1.0, 2.0, 3.0]); ax /= np.linalg.norm(ax)
    q = np.concatenate([[math.cos(0.35)], ax * math.sin(0.35)]).astype(f32).astype(np.float64)
    w, x, y, z = q
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                  [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                  [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])
    M = np.eye(4)
    M[:3, :3] = R * 2.0
    M[:3, 3] = [1.0, -2.0, 3.0]
    M = M.astype(f32)
    L = np.ascontiguousarray(M[:3, :3].T)
    t = M[:3, 3].copy()
    out = np.empty((m, 3), dtype=f32)

    def f():
        for _ in range(passes):
            np.matmul(p, L, out=out)
            np.add(out, t, out=out)
        return out
    st, o = timed(f)
    cs = float(o.astype(np.float64).sum())
    emit("transform-apply", m * passes, "numpy", NPV, st, abs(cs - 1910391.7256280927) <= 2e-5 * m,
         smoke_note("10 passes in place of 100") + f"{m * passes:.0e} point applications as {passes} passes over 1e6 points; float32 points @ 3x3 of the 4x4 + translation; check: |checksum-ref| <= 2e-5*m")


BENCHES = [
    ("tensor-element-get", b_element_get), ("tensor-element-set", b_element_set),
    ("tensor-elementwise", b_elementwise), ("tensor-broadcast", b_broadcast),
    ("tensor-reduce", b_reduce), ("tensor-transpose", b_transpose), ("tensor-cast", b_cast),
    ("linalg-solve", lambda: b_linalg("linalg-solve")), ("linalg-inverse", lambda: b_linalg("linalg-inverse")),
    ("linalg-qr", lambda: b_linalg("linalg-qr")), ("linalg-svd", lambda: b_linalg("linalg-svd")),
    ("fft-1d", b_fft1d), ("fft-2d", b_fft2d),
    ("random-uniform", lambda: b_random("random-uniform")), ("random-normal", lambda: b_random("random-normal")),
    ("npy-load-i32", b_npy_load), ("npy-save-i32", b_npy_save),
    ("poly-eval", b_poly_eval), ("poly-fit", b_poly_fit),
    ("stats-moments", b_moments), ("stats-histogram", b_histogram),
    ("transform-apply", b_transform),
]


def main():
    for slug, fn in BENCHES:
        if not wanted(slug):
            continue
        try:
            fn()
        except Exception as e:  # a failed row, then the next id
            traceback.print_exc(file=sys.stderr)
            print(row(slug, 0, "numpy", NPV, 0, 0, (-1, -1, -1, -1), "failed", None,
                      f"{type(e).__name__}: {e}"), flush=True)


if __name__ == "__main__":
    main()
