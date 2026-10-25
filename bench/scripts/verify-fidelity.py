#!/usr/bin/env python3
# Copy of cajeta-bench tools/verify-fidelity.py as of dev.cajeta.bench 0.1.5,
# the source of truth; refresh it when the library moves. bench.sh runs it.
# Matches 0.1.5 exactly (0.1.1: math/matmul by id and the `external`-row
# exemption; 0.1.3: UTF-8; 0.1.5: absolute ceilings keyed by machine).
# Verification gate over a benchmark results.csv, schema 1 or 2.
#
# Two layers:
#
#  1. SCHEMA (always). The header is one this gate knows, every row's
#     schema_version agrees with it, every status is a known one, and on
#     schema 2 every row names its machine and platform (spec §3.2: a row that
#     cannot say where and by what it was measured is not publishable).
#       schema 1 — the samples/profile columns: 32 (… notes,variant,backend),
#                  or the older 31 (no backend) and 30 (no variant either)
#       schema 2 — schema 1's 32 in order, then machine,os,driver,model,platform
#
#  2. FIDELITY (benchmark-fidelity Unit 11.a.1). The outcomes of the fidelity
#     plan, each checked when its benchmark is in the file:
#       * Memory tab populated  — no measured Cajeta row has alloc_bytes == -1
#                                  (rows tagged `external` excepted)
#       * string-build fairness — every language reports input_size == 4000
#       * JSON float parsing     — Cajeta json-dom/serialize/roundtrip rows are ok
#       * matmul kernel-pool     — Cajeta math/matmul median in the kernel-pool band
#       * SIMD compute kernels    — dot/saxpy/spectral improved vs the scalar baselines
#     The last two are ABSOLUTE and read the row's machine's table (CEILINGS);
#     a machine without one is skipped. The first three are machine-independent.
#     A benchmark absent from the file is reported SKIP, so a consumer repo's
#     one-benchmark CSV passes. `--require-suite` restores the profile gate's
#     behavior, where an absent benchmark is a FAIL; the compiler repo's
#     bench.sh passes it over the whole catalog.
#
# Usage: verify-fidelity.py [--require-suite] [--no-smoke] <results-dir-or-csv>
#   --no-smoke   fail when any row is tagged `smoke` (the publish step, §9.3)
# Exit 0 = all checks pass; 1 = at least one failed (details printed); 2 = usage.

import csv
import os
import sys

# Absolute ceilings, per machine. Everything else in this gate is relative or
# structural and machine-independent.
#
#   proton   the ceilings the fidelity plan set (Ryzen AI MAX+ 395): the
#            kernel-pool matmul band and the scalar baselines the SIMD kernels
#            must beat (dot ~390 us, saxpy ~590 us, spectral ~1632 us scalar).
#   phoenix  PROVISIONAL (Ryzen 9 3950X, WSL2). Nothing is measured for
#            publication yet, so these are 2x the smoke medians the compiler
#            repo's bench/build/bench reported on 2026-10-25 18:07 (cajeta
#            0.46.3, CAJETA_BENCH_SMOKE=1, warmup 1, trials 3, load average
#            1.1): saxpy 1,404,384; dot-product 692,591; clbg-spectral-norm
#            164,395; math/matmul 176,335 ns (band 0.25x to 2x). Replace them
#            from the first quiet-box run.
LEGACY_MACHINE = "proton"   # the machine a v1 row (no machine column) is judged on
CEILINGS = {
    "proton": {
        "matmul_band": (40000, 130000),
        "simd": {"dot-product": 250000, "saxpy": 420000, "clbg-spectral-norm": 700000},
    },
    "phoenix": {
        "matmul_band": (44000, 353000),
        "simd": {"dot-product": 1390000, "saxpy": 2810000, "clbg-spectral-norm": 330000},
    },
}

V1_COLUMNS = (
    "schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,"
    "dataset_sha256,input_size,language,language_version,library,library_version,flags,"
    "warmup,trials,min_ns,median_ns,mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,"
    "cgroup_peak_kb,alloc_bytes,alloc_count,working_set_kb,status,check_ok,notes,variant,backend"
).split(",")
V2_COLUMNS = V1_COLUMNS + ["machine", "os", "driver", "model", "platform"]

# Every header shape each schema version has shipped with.
KNOWN_HEADERS = {
    1: [V1_COLUMNS, V1_COLUMNS[:-1], V1_COLUMNS[:-2]],
    2: [V2_COLUMNS],
}
STATUSES = {1: {"ok", "skipped", "invalid"}, 2: {"ok", "skipped", "invalid", "failed"}}


def is_cpu_matmul(r):
    """The CPU kernel-pool matmul is the catalog id math/matmul, never the bare
    name `matmul`: the device GEMMs (xpu/gemm-f32, and v1's gpu/matmul) are other
    benchmarks and must not be read against the CPU band."""
    return r["benchmark"] == "matmul" and r["area"] == "math"


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "results.csv")
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        return list(reader.fieldnames or []), rows


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def header_schema(header):
    for version, shapes in KNOWN_HEADERS.items():
        if header in shapes:
            return version
    return None


def is_smoke(row):
    return "smoke" in [p.strip() for p in (row.get("notes") or "").split(";")]


def main(argv):
    require_suite = "--require-suite" in argv
    no_smoke = "--no-smoke" in argv
    paths = [a for a in argv if not a.startswith("--")]
    unknown = [a for a in argv if a.startswith("--") and a not in ("--require-suite", "--no-smoke")]
    if len(paths) != 1 or unknown:
        print("usage: verify-fidelity.py [--require-suite] [--no-smoke] <results-dir-or-csv>",
              file=sys.stderr)
        return 2
    header, rows = load(paths[0])
    failures = []

    def check(name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")
        if not ok:
            failures.append(name)

    def skip(name, why):
        print(f"  [SKIP] {name} — {why}")

    # ---- 1. schema ----------------------------------------------------------
    schema = header_schema(header)
    check("schema: header is a known schema 1 or 2 shape", schema is not None,
          f"schema {schema}, {len(header)} columns" if schema else
          f"{len(header)} columns, first unknown: "
          + next((f"{i}:{c}" for i, c in enumerate(header)
                  if i >= len(V2_COLUMNS) or c != V2_COLUMNS[i]), "(prefix of v2)"))
    if schema is None:
        print(f"\nFAILED: unrecognized header")
        return 1
    bad_ver = [r for r in rows if r["schema_version"] != str(schema)]
    check(f"schema: every row is schema_version {schema}", not bad_ver,
          f"{len(bad_ver)} rows disagree" if bad_ver else f"{len(rows)} rows")
    bad_status = sorted({r["status"] for r in rows} - STATUSES[schema])
    check("schema: every status is known", not bad_status,
          "unknown: " + ", ".join(bad_status) if bad_status else "")
    if schema == 2:
        for col in ("machine", "platform"):
            empty = [r for r in rows if not (r[col] or "").strip()]
            check(f"schema 2: every row names its {col}", not empty,
                  f"{len(empty)} rows empty, e.g. {empty[0]['benchmark']}" if empty else "")
    if no_smoke:
        smoke = [r for r in rows if is_smoke(r)]
        check("publish: no row is tagged smoke", not smoke,
              f"{len(smoke)} smoke rows" if smoke else "")

    # ---- 2. fidelity ----------------------------------------------------------
    caj = [r for r in rows if r["language"] == "cajeta"]
    measured = [r for r in caj if r["status"] == "ok"]
    present = {r["benchmark"] for r in rows}

    def gated(name, benches, fn):
        if require_suite or any(b in present for b in benches):
            fn()
        else:
            skip(name, "not in this file")

    # 1. Memory tab — no measured Cajeta row emits the -1 alloc_bytes sentinel.
    #    Except an ExternalBenchmark row (notes tag `external`, dev.cajeta.bench
    #    0.1.1): its work runs in a child process, its memory is the child's
    #    peak_rss_kb, and the harness has no allocator count to give.
    def is_external(r):
        return "external" in [p.strip() for p in (r.get("notes") or "").split(";")]
    bad_alloc = [r for r in measured if not is_external(r)
                 and (num(r["alloc_bytes"]) is None or num(r["alloc_bytes"]) < 0)]
    check("memory: no measured Cajeta alloc_bytes == -1", not bad_alloc,
          "" if not bad_alloc else f"{len(bad_alloc)} offenders, e.g. "
          + ", ".join(sorted({r["benchmark"] for r in bad_alloc})[:5]))

    # 2. string-build-concat — every language reports the equal 4000-byte workload.
    def string_build():
        sb = [r for r in rows if r["benchmark"] == "string-build-concat" and r["status"] == "ok"]
        sizes = {r["language"]: r["input_size"] for r in sb}
        check("fairness: string-build input_size == 4000 for all languages",
              bool(sb) and all(num(v) == 4000 for v in sizes.values()),
              f"sizes={sizes}")
    gated("fairness: string-build input_size == 4000 for all languages",
          ["string-build-concat"], string_build)

    # 3. JSON float parsing — Cajeta json benches are ok (not float-parse skips).
    for bench in ("json-dom", "json-serialize", "json-roundtrip"):
        def json_check(bench=bench):
            jr = [r for r in caj if r["benchmark"] == bench]
            oks = [r for r in jr if r["status"] == "ok"]
            skips_float = [r for r in jr if r["status"] == "skipped" and "float" in r["notes"].lower()]
            check(f"json: {bench} Cajeta rows ok (no float-parse skip)",
                  bool(oks) and not skips_float,
                  f"{len(oks)} ok, {len(skips_float)} float-skips")
        gated(f"json: {bench} Cajeta rows ok (no float-parse skip)", [bench], json_check)

    # 4-5. ABSOLUTE checks, keyed by the row's machine (CEILINGS above). A
    #      machine with no table is skipped, never judged by another's; a v1
    #      row (no machine column) reads LEGACY_MACHINE's.
    machines = sorted({(r.get("machine") or LEGACY_MACHINE) for r in measured})
    for m in machines:
        table = CEILINGS.get(m)
        on_m = [r for r in measured if (r.get("machine") or LEGACY_MACHINE) == m]
        if table is None:
            skip(f"absolute ceilings on {m}", "no ceiling table for this machine")
            continue

        lo, hi = table["matmul_band"]

        def matmul(on_m=on_m, m=m, lo=lo, hi=hi):
            mm = [r for r in on_m if is_cpu_matmul(r)]
            mm_med = num(mm[0]["median_ns"]) if mm else None
            check(f"matmul@{m}: Cajeta median in kernel-pool band ({lo}–{hi} ns)",
                  mm_med is not None and lo <= mm_med <= hi,
                  f"median_ns={mm_med}")
        if require_suite or any(is_cpu_matmul(r) for r in on_m):
            matmul()
        else:
            skip(f"matmul@{m}: Cajeta median in kernel-pool band ({lo}–{hi} ns)", "not in this file")

        # SIMD compute kernels — improved vs the recorded scalar baselines.
        for bench, ceiling in table["simd"].items():
            def simd(bench=bench, ceiling=ceiling, on_m=on_m, m=m):
                b = [r for r in on_m if r["benchmark"] == bench]
                med = num(b[0]["median_ns"]) if b else None
                check(f"simd@{m}: {bench} median < {ceiling} ns (beats scalar baseline)",
                      med is not None and med < ceiling,
                      f"median_ns={med}")
            if require_suite or any(r["benchmark"] == bench for r in on_m):
                simd()
            else:
                skip(f"simd@{m}: {bench} median < {ceiling} ns (beats scalar baseline)", "not in this file")

    print(f"\n{'OK' if not failures else 'FAILED'}: "
          f"{len(failures)} failing check(s)" + (": " + ", ".join(failures) if failures else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
