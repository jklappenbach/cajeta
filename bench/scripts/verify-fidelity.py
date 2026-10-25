#!/usr/bin/env python3
# benchmark-fidelity Unit 11 (11.a.1) — verification gate over a results.csv.
#
# Asserts the fidelity outcomes of the benchmark-fidelity plan are present in a
# freshly-generated run, so a regression in any of them fails CI / the republish
# rather than silently shipping:
#   * Memory tab populated  — no measured Cajeta row has alloc_bytes == -1
#   * string-build fairness — every language reports input_size == 4000
#   * JSON float parsing     — Cajeta json-dom/serialize/roundtrip rows are ok
#   * matmul kernel-pool     — Cajeta matmul median in the kernel-pool band
#   * SIMD compute kernels    — dot/saxpy/spectral improved vs the scalar baselines
#
# Usage: verify-fidelity.py <results-dir-or-csv>
# Exit 0 = all checks pass; 1 = at least one failed (details printed).

import csv
import os
import sys


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "results.csv")
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def main():
    if len(sys.argv) != 2:
        print("usage: verify-fidelity.py <results-dir-or-csv>", file=sys.stderr)
        return 2
    rows = load(sys.argv[1])
    caj = [r for r in rows if r["language"] == "cajeta"]
    measured = [r for r in caj if r["status"] == "ok"]
    failures = []

    def check(name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")
        if not ok:
            failures.append(name)

    # 1. Memory tab — no measured Cajeta row emits the -1 alloc_bytes sentinel.
    bad_alloc = [r for r in measured if (num(r["alloc_bytes"]) is None or num(r["alloc_bytes"]) < 0)]
    check("memory: no measured Cajeta alloc_bytes == -1", not bad_alloc,
          "" if not bad_alloc else f"{len(bad_alloc)} offenders, e.g. "
          + ", ".join(sorted({r["benchmark"] for r in bad_alloc})[:5]))

    # 2. string-build-concat — every language reports the equal 4000-byte workload.
    sb = [r for r in rows if r["benchmark"] == "string-build-concat" and r["status"] == "ok"]
    sizes = {r["language"]: r["input_size"] for r in sb}
    check("fairness: string-build input_size == 4000 for all languages",
          bool(sb) and all(num(v) == 4000 for v in sizes.values()),
          f"sizes={sizes}")

    # 3. JSON float parsing — Cajeta json benches are ok (not float-parse skips).
    for bench in ("json-dom", "json-serialize", "json-roundtrip"):
        jr = [r for r in caj if r["benchmark"] == bench]
        oks = [r for r in jr if r["status"] == "ok"]
        skips_float = [r for r in jr if r["status"] == "skipped" and "float" in r["notes"].lower()]
        check(f"json: {bench} Cajeta rows ok (no float-parse skip)",
              bool(oks) and not skips_float,
              f"{len(oks)} ok, {len(skips_float)} float-skips")

    # 4. matmul — Cajeta median in the kernel-pool band (~0.05–0.08 ms; allow slack).
    mm = [r for r in measured if r["benchmark"] == "matmul"]
    mm_med = num(mm[0]["median_ns"]) if mm else None
    check("matmul: Cajeta median in kernel-pool band (40k–130k ns)",
          mm_med is not None and 40000 <= mm_med <= 130000,
          f"median_ns={mm_med}")

    # 5. SIMD compute kernels — improved vs the recorded scalar baselines.
    #    (dot ~390us, saxpy ~590us, spectral ~1632us before vectorization.)
    targets = {"dot-product": 250000, "saxpy": 420000, "clbg-spectral-norm": 700000}
    for bench, ceiling in targets.items():
        b = [r for r in measured if r["benchmark"] == bench]
        med = num(b[0]["median_ns"]) if b else None
        check(f"simd: {bench} median < {ceiling} ns (beats scalar baseline)",
              med is not None and med < ceiling,
              f"median_ns={med}")

    print(f"\n{'OK' if not failures else 'FAILED'}: "
          f"{len(failures)} failing check(s)" + (": " + ", ".join(failures) if failures else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
