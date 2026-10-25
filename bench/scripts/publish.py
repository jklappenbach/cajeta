#!/usr/bin/env python3
"""publish.py — bench.sh publish's work (spec §9.4, plan 22.1.1). Run through
scripts/publish.sh, which supplies the defaults.

  publish.py --machine M --run-id R --src DIR --repos "name=path ..." \
             --dest DIR --bench DIR --site DIR

Collects the compiler repo's run (--src: results.csv, env.csv) and each
ecosystem repo's (<path>/bench/results/<M>/<R>/) into <dest>/<M>/<R>/:

  results.csv  every row, schema v2 only, ids as they are (the area is already
               the package: llm/decode, ml/knn-fit), smoke rows dropped. A row
               must be on machine M, and an ecosystem repo's row must be in its
               package (REPO_PACKAGES) or an id bench/ecosystem-ids.txt gives
               that repo; one id from two sources is refused.
  env.csv      every source's env.csv lines under a key,value,source header
               (source `cajeta` for the compiler repo), plus a `publish` line
               per source with its row count or `absent`.

then runs scripts/verify-fidelity.py --require-suite --no-smoke on the merged
file and, only when it passes, moves the run into place and writes
<dest>/<M>/LATEST. A run directory that already exists is never overwritten.
Prints the site regen command; regenerating and deploying are separate steps.

Exit 0 published; 1 refused (nothing written); 2 usage.
"""
import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile

V2 = ("schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,dataset_sha256,"
      "input_size,language,language_version,library,library_version,flags,warmup,trials,min_ns,median_ns,"
      "mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,cgroup_peak_kb,alloc_bytes,alloc_count,"
      "working_set_kb,status,check_ok,notes,variant,backend,machine,os,driver,model,platform").split(",")
CATALOG = set("lang collection concurrent time hash codec io math nucleo wire search process reflect aot xpu gfx toolchain".split())
# The package each ecosystem repo's rows belong to (spec §7). cajeta-codec has
# no package of its own here: its rows are the ids ecosystem-ids.txt gives it.
REPO_PACKAGES = {
    "cajeta-llm": {"llm"}, "cajeta-ml": {"ml"}, "cajeta-xgboost": {"ml"},
    "cajeta-http": {"http"}, "cajeta-primavera": {"primavera"}, "cajeta-codec": set(),
}
RUN_ID = re.compile(r"^\d{8}T\d{4}Z-[0-9A-Za-z.+-]+$")
MACHINE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")


class Refused(Exception):
    pass


def is_smoke(row):
    return "smoke" in [p.strip() for p in (row.get("notes") or "").split(";")]


def read_results(path, source):
    with open(path, newline="", encoding="utf-8") as f:
        rd = csv.reader(f)
        header = next(rd, None)
        if header != V2:
            raise Refused(f"{source}: {path} is not schema v2 ({len(header or [])} columns; publish takes schema v2 only)")
        rows = []
        for n, rec in enumerate(rd, start=2):
            if not rec:
                continue
            if len(rec) != len(V2):
                raise Refused(f"{source}: {path} line {n} has {len(rec)} columns, not {len(V2)}")
            rows.append(dict(zip(V2, rec)))
        return rows


def read_env(path):
    if not os.path.exists(path):
        return []
    out = []
    with open(path, newline="", encoding="utf-8") as f:
        for rec in csv.reader(f):
            if not rec or not rec[0] or rec[:2] == ["key", "value"]:
                continue
            out.append((rec[0], ",".join(rec[1:])))
    return out


def ecosystem_ids(bench):
    ids = {}
    p = os.path.join(bench, "ecosystem-ids.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            parts = line.split()
            if parts and not parts[0].startswith("#") and len(parts) >= 2:
                ids[parts[0]] = parts[1]
    return ids


def main(argv):
    ap = argparse.ArgumentParser(add_help=False)
    for k in ("machine", "run-id", "src", "repos", "dest", "bench", "site"):
        ap.add_argument("--" + k, required=True)
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print("usage: bench.sh publish <machine> <run-id>", file=sys.stderr)
        return 2
    if not MACHINE.match(a.machine) or not RUN_ID.match(a.run_id):
        print(f"usage: bench.sh publish <machine> <run-id>: machine is a lowercase name, run-id "
              f"<ISO-8601-basic-UTC>-<cajeta-version> (20261101T0300Z-0.46.3); got '{a.machine}' '{a.run_id}'",
              file=sys.stderr)
        return 2
    repos = []
    for item in a.repos.split():
        name, _, path = item.partition("=")
        if not path:
            print(f"publish: BENCH_PUBLISH_REPOS entry '{item}' is not name=path", file=sys.stderr)
            return 2
        repos.append((name, path))

    final = os.path.join(a.dest, a.machine, a.run_id)
    try:
        if os.path.exists(final):
            raise Refused(f"{final} is already published; a run directory is never overwritten")
        src_csv = os.path.join(a.src, "results.csv")
        if not os.path.exists(src_csv):
            raise Refused(f"no compiler-repo run at {a.src} (results.csv; BENCH_PUBLISH_SRC overrides)")
        eco = ecosystem_ids(a.bench)
        sources = [("cajeta", a.src)] + [(n, os.path.join(p, "bench", "results", a.machine, a.run_id)) for n, p in repos]
        rows, env, seen, counts, smoke = [], [], {}, {}, 0
        for name, d in sources:
            path = os.path.join(d, "results.csv")
            if name != "cajeta" and not os.path.exists(path):
                counts[name] = "absent"
                print(f"[publish] {name}: absent (no {path})")
                continue
            kept = 0
            for r in read_results(path, name):
                if is_smoke(r):
                    smoke += 1
                    continue
                rid = f"{r['area']}/{r['benchmark']}"
                if r["machine"] != a.machine:
                    raise Refused(f"{name}: row {rid} is from machine {r['machine'] or '(none)'}, not {a.machine}")
                if name == "cajeta":
                    if r["area"] not in CATALOG:
                        raise Refused(f"cajeta: row {rid} is not in a catalog package")
                elif r["area"] not in REPO_PACKAGES.get(name, set()) and eco.get(rid) != name:
                    raise Refused(f"{name}: row {rid} is not {name}'s (its packages: "
                                  f"{', '.join(sorted(REPO_PACKAGES.get(name, set()))) or 'none'}; "
                                  f"ecosystem-ids.txt gives it {', '.join(sorted(i for i, rp in eco.items() if rp == name)) or 'nothing'})")
                if name == "cajeta" and eco.get(rid):
                    raise Refused(f"cajeta: row {rid} belongs to {eco[rid]} (ecosystem-ids.txt)")
                if rid in seen and seen[rid] != name:
                    raise Refused(f"{rid} comes from two sources, {seen[rid]} and {name} (an id is published once, twice is a collision)")
                seen[rid] = name
                rows.append(r)
                kept += 1
            counts[name] = kept
            for k, v in read_env(os.path.join(d, "env.csv")):
                env.append((k, v, name))
            print(f"[publish] {name}: {kept} rows from {path}")

        os.makedirs(os.path.join(a.dest, a.machine), exist_ok=True)
        tmp = tempfile.mkdtemp(prefix=f".{a.run_id}.", dir=os.path.join(a.dest, a.machine))
        try:
            with open(os.path.join(tmp, "results.csv"), "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(V2)
                for r in rows:
                    w.writerow([r[c] for c in V2])
            with open(os.path.join(tmp, "env.csv"), "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(["key", "value", "source"])
                for k, v, s in env:
                    w.writerow([k, v, s])
                for s, n in counts.items():
                    w.writerow(["publish", f"{n} rows" if n != "absent" else "absent", s])
            gate = [sys.executable, os.path.join(a.bench, "scripts", "verify-fidelity.py"),
                    "--require-suite", "--no-smoke", os.path.join(tmp, "results.csv")]
            print(f"[publish] verify-fidelity --require-suite --no-smoke ({len(rows)} rows)")
            g = subprocess.run(gate, capture_output=True, text=True)
            sys.stdout.write("".join("  " + l + "\n" for l in g.stdout.splitlines()))
            if g.returncode != 0:
                raise Refused(f"verify-fidelity --require-suite --no-smoke failed (exit {g.returncode}); nothing published")
            for fn in ("results.csv", "env.csv"):
                os.chmod(os.path.join(tmp, fn), 0o644)
            os.chmod(tmp, 0o755)
            os.rename(tmp, final)
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        with open(os.path.join(a.dest, a.machine, "LATEST.tmp"), "w", encoding="utf-8") as f:
            f.write(a.run_id + "\n")
        os.replace(os.path.join(a.dest, a.machine, "LATEST.tmp"), os.path.join(a.dest, a.machine, "LATEST"))
    except Refused as e:
        print(f"[publish] refused: {e}", file=sys.stderr)
        return 1
    print(f"[publish] {len(rows)} rows ({smoke} smoke rows dropped) -> {final}; LATEST = {a.run_id}")
    print(f"[publish] regenerate the site: (cd {a.site} && npm run regen)   # deploy is a separate step")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
