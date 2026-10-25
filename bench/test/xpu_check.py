#!/usr/bin/env python3
"""Row assertions for the xpu/ and gfx/ smokes (benchmark-reference unit 14).

  xpu_check.py rows <csv> <expect>   expect: ok | held | norocm
      ok      every row of a page id is status ok, check_ok true, min_ns > 0,
              tagged smoke -- except the skips this unit justifies (the
              showcase's other-backend variants, mesh-simplify 1e6-to-1e5);
      held    every row is skipped with "GPU held by another session";
      norocm  every row is skipped with "no ROCm on this machine".
      Every id on the pages must have a row. Prints one line per row.
  xpu_check.py ids <bench-dir>       print the page ids (area/slug), one per line
  xpu_check.py listed <bench-dir> <list-output>   every page id is in --list
  xpu_check.py invalid <csv> <id> <variant>       that row is status invalid
  xpu_check.py comp <csv> <expect>   competitor rows (v2): expect build|held|run
"""
import csv
import glob
import os
import re
import sys

PAGES = ["xpu/Device.md", "xpu/KernelBuffer.md", "xpu/KernelStream.md", "xpu/KernelTimer.md",
         "xpu/mesh/MeshSimplifier.md", "gfx/Texture2D.md"]
JUSTIFIED = {("cross-backend-showcase", "cuda"), ("cross-backend-showcase", "hip"),
             ("cross-backend-showcase", "vulkan"), ("mesh-simplify", "1e6-to-1e5")}


def page_ids(bench):
    ids = []
    for p in PAGES:
        for line in open(os.path.join(bench, p)):
            m = re.match(r"^\| `((?:xpu|gfx)/[a-z0-9-]+)`", line)
            if m and m.group(1) not in ids:
                ids.append(m.group(1))
    return ids


def fail(msg):
    print("XPU CHECK FAIL: " + msg, file=sys.stderr)
    sys.exit(1)


def rows(path):
    return list(csv.DictReader(open(path, newline="")))


def main():
    cmd = sys.argv[1]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ids = page_ids(here)
    if cmd == "ids":
        print("\n".join(ids))
        return
    if cmd == "listed":
        text = open(sys.argv[3]).read()
        for i in ids:
            if not re.search(r"(^|\s)" + re.escape(i) + r"(\s|$)", text, re.M):
                fail(f"{i} not in --list")
        print(f"all {len(ids)} page ids listed")
        return
    if cmd == "invalid":
        for r in rows(sys.argv[2]):
            if r["benchmark"] == sys.argv[3] and r["variant"] == sys.argv[4]:
                if r["status"] != "invalid":
                    fail(f"{sys.argv[3]} [{sys.argv[4]}] status {r['status']} (want invalid): {r['notes']}")
                print(f"{sys.argv[3]} [{sys.argv[4]}] invalid as it must be: {r['notes'][:120]}")
                return
        fail(f"no row {sys.argv[3]} [{sys.argv[4]}]")
    if cmd == "rows":
        rs, expect = rows(sys.argv[2]), sys.argv[3]
        seen = set()
        for r in rs:
            rid = r["area"] + "/" + r["benchmark"]
            if rid not in ids:
                fail(f"row for {rid} which is on no page")
            seen.add(rid)
            v, st, ok, notes = r["variant"], r["status"], r["check_ok"], r["notes"]
            tag = f"{rid}{' [' + v + ']' if v else ''}"
            if "smoke" not in notes:
                fail(f"{tag} not tagged smoke")
            host_row = r["benchmark"] == "mesh-simplify"   # MeshSimplifier is host code: cpu in every process
            if expect == "ok" or host_row:
                if (r["benchmark"], v) in JUSTIFIED:
                    if st != "skipped" or not notes:
                        fail(f"{tag} should be a skipped row with a reason, got {st}")
                elif st != "ok" or ok != "true":
                    fail(f"{tag} status={st} check_ok={ok} notes={notes}")
                elif not re.fullmatch(r"[0-9]+", r["min_ns"]) or int(r["min_ns"]) <= 0:
                    fail(f"{tag} min_ns={r['min_ns']} (elided?)")
            else:
                want = {"held": "GPU held by another session (CAJETA_BENCH_NO_GPU=1)",
                        "norocm": "no ROCm on this machine"}[expect]
                if st != "skipped":
                    fail(f"{tag} status={st} (want skipped: {want})")
                own = r["benchmark"] != "cross-backend-showcase" or v == r["backend"]
                if own and want not in notes:
                    fail(f"{tag} skip reason is not '{want}': {notes}")
            print(f"  {st:8s} {tag:52s} backend={r['backend']:6s} min_ns={r['min_ns']}")
        for i in ids:
            if i not in seen:
                fail(f"page id {i} has no row")
        print(f"{len(rs)} rows, all {len(ids)} page ids, expectation '{expect}' holds")
        return
    if cmd == "comp":
        rs, expect = rows(sys.argv[2]), sys.argv[3]
        ncols = len(open(os.path.join(here, "competitors/columns.txt")).readline().strip().split(","))
        raw = [l for l in open(sys.argv[2]).read().splitlines() if l.strip()]
        for l in raw[1:]:
            if len(next(csv.reader([l]))) != ncols:
                fail(f"row is not {ncols} columns: {l[:160]}")
        cover = {}
        for r in rs:
            if r["schema_version"] != "2" or not r["machine"] or not r["os"] or not r["platform"]:
                fail(f"bad v2 identity: {r}")
            rid = r["area"] + "/" + r["benchmark"]
            if rid not in ids:
                fail(f"competitor row for {rid}, on no page")
            key = (r["language"], r["library"], r["backend"])
            cover.setdefault(key, set()).add(rid)
            st, notes = r["status"], r["notes"]
            if r["library"] == "meshoptimizer":
                if st != "ok" or r["check_ok"] != "true" or int(r["min_ns"]) <= 0:
                    fail(f"meshoptimizer row {r['variant']} status={st} check={r['check_ok']}")
            elif r["backend"] == "hip":
                if st != "skipped" or "no ROCm on this machine" not in notes:
                    fail(f"hip row {rid} {r['library']} not skipped 'no ROCm': {st} {notes}")
            elif expect == "build":
                if st != "skipped" or not notes.startswith("build-only: ") or "failed" in notes:
                    fail(f"{rid} {r['language']} {r['library']}: {st} {notes} (want build-only: built)")
            elif expect == "held":
                if st != "skipped" or "GPU held by another session" not in notes:
                    fail(f"{rid} {r['language']} {r['library']}: {st} {notes} (want GPU held)")
            else:
                if st not in ("ok", "skipped") or (st == "ok" and r["check_ok"] != "true"):
                    fail(f"{rid} {r['language']} {r['library']}: {st} check={r['check_ok']} {notes}")
        for k in sorted(cover):
            print(f"  {k[0]:9s} {k[1]:24s} {k[2]:5s} {len(cover[k]):2d} ids")
        print(f"{len(rs)} competitor rows, expectation '{expect}' holds")
        return
    fail("usage")


if __name__ == "__main__":
    main()
