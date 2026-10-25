#!/usr/bin/env bash
# Coverage smoke (benchmark-reference plan 4.1.1, spec §6.0). Static: no build,
# no run. Asserts:
#  (1) every stdlib class page docs/stdlib/<pkg>/**/<Class>.md has a benchmark
#      page bench/<pkg>/**/<Class>.md or a row in bench/EXEMPTIONS.md
#      (| `<pkg>/<path>/<Class>` | <reason> |) with a reason;
#  (2) every exemption names a stdlib page that exists and has no benchmark
#      page (a stale or contradicted exemption fails);
#  (3) every benchmark page (bench/<pkg>/**/*.md but README.md, in the catalog
#      and ecosystem packages; toolchain/kernel-startup/FINDINGS.md is notes)
#      has at least one ```bench or ```bench-matrix block;
#  (4) every id a block names is registered (bench/registered-ids.txt, the
#      union of every build's --list) or belongs to an ecosystem package that
#      registers in its own repo: llm, ml, http, primavera, and the ids in
#      bench/ecosystem-ids.txt (cajeta-codec's four deflate rows, on
#      codec/Deflate.md). No `pending:` allowance:
#      an id nothing registers fails;
#  (5) every registered id is on some page.
# Prints the counts it checked.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"

python3 - "$PROJ" "$REPO_ROOT/docs/stdlib" <<'EOF'
import os, re, sys
proj, stdlib = sys.argv[1], sys.argv[2]
CATALOG = "lang collection concurrent time hash codec io math nucleo wire search process reflect aot xpu gfx toolchain".split()
ECOSYSTEM = "llm ml http primavera".split()
# ids measured in another repo's bench/ on a covered package's page (spec §7,
# §9.4): bench/ecosystem-ids.txt, the list the site reads too.
ECOSYSTEM_IDS = set()
eco_file = os.path.join(proj, "ecosystem-ids.txt")
if os.path.exists(eco_file):
    for l in open(eco_file, encoding="utf-8"):
        if l.strip() and not l.lstrip().startswith("#"):
            i = l.split()[0]
            if i.split("/")[0] not in CATALOG: fail(f"ecosystem-ids.txt: {i} is not in a catalog package (list whole ecosystem packages in ECOSYSTEM instead)")
            ECOSYSTEM_IDS.add(i)
# Notes that are not benchmark pages: the kernel-startup tool's findings (its
# README is the tool's page; the measured id is toolchain/kernel-startup).
NOTES = {"toolchain/kernel-startup/FINDINGS.md"}
fails = []
def fail(m): fails.append(m)

def md_under(root):
    out = []
    for d, ds, fs in os.walk(root):
        ds[:] = [x for x in ds if not x.startswith(".")]
        out += [os.path.join(d, f) for f in fs if f.endswith(".md")]
    return sorted(out)

# (1) stdlib class pages
bench_by_pkg = {}
for pkg in CATALOG + ECOSYSTEM:
    root = os.path.join(proj, pkg)
    bench_by_pkg[pkg] = [os.path.relpath(p, proj) for p in md_under(root)] if os.path.isdir(root) else []
ex = {}
for line in open(os.path.join(proj, "EXEMPTIONS.md"), encoding="utf-8"):
    m = re.match(r"^\|\s*`([a-z0-9_]+/[A-Za-z0-9_/]+)`\s*\|\s*(.*?)\s*\|\s*$", line)
    if m:
        if m.group(1) in ex: fail(f"EXEMPTIONS.md lists {m.group(1)} twice")
        ex[m.group(1)] = m.group(2)
stdlib_pages = []
for p in md_under(stdlib):
    rel = os.path.relpath(p, stdlib)
    if "/" not in rel or os.path.basename(rel) == "README.md":
        continue   # the package READMEs and the top-level worklists are not class pages
    stdlib_pages.append(rel[:-3])
def has_page(key):
    pkg, base = key.split("/")[0], key.split("/")[-1] + ".md"
    return key + ".md" in bench_by_pkg.get(pkg, []) or any(os.path.basename(b) == base for b in bench_by_pkg.get(pkg, []))
covered = exempt = 0
for key in stdlib_pages:
    if has_page(key):
        covered += 1
    elif key in ex:
        exempt += 1
        if not ex[key] or ex[key] == "-": fail(f"EXEMPTIONS.md: {key} has no reason")
    else:
        fail(f"docs/stdlib/{key}.md has no bench page (bench/{key}.md) and no EXEMPTIONS.md row")
# (2) exemptions
for key in ex:
    if key not in stdlib_pages: fail(f"EXEMPTIONS.md: {key} names no docs/stdlib page (stale)")
    elif has_page(key): fail(f"EXEMPTIONS.md: {key} is exempt but bench/{key}.md exists")

# (3)/(4) blocks and ids
reg_file = os.path.join(proj, "registered-ids.txt")
if not os.path.exists(reg_file):
    fail("no bench/registered-ids.txt (./bench.sh --registered-ids)")
    reg = []
else:
    reg = [l.split()[0] for l in open(reg_file, encoding="utf-8") if l.strip() and not l.lstrip().startswith("#")]
regset = set(reg)
page_ids, npages, nblocks = {}, 0, 0
for pkg in CATALOG + ECOSYSTEM:
    for rel in bench_by_pkg[pkg]:
        text = open(os.path.join(proj, rel), encoding="utf-8").read()
        blocks = re.findall(r"^```(bench|bench-matrix)[ \t]*\n(.*?)^```", text, re.M | re.S)
        if os.path.basename(rel) != "README.md" and rel not in NOTES:
            npages += 1
            if not blocks: fail(f"bench/{rel} has no ```bench block")
        for kind, body in blocks:
            nblocks += 1
            for line in body.splitlines():
                m = re.match(r"\s*(id|ids)\s*:\s*(.*)$", line)
                if not m: continue
                for i in [x.strip() for x in m.group(2).split(",") if x.strip()]:
                    page_ids.setdefault(i, rel)
                    if i in regset: continue
                    if i.split("/")[0] in ECOSYSTEM or i in ECOSYSTEM_IDS: continue
                    fail(f"bench/{rel}: id {i} is not registered (bench/registered-ids.txt) and is no ecosystem package's")
# (5) registered ids on pages; an ecosystem id is on a page and not also registered here
for i in reg:
    if i not in page_ids: fail(f"registered id {i} is on no page")
for i in sorted(ECOSYSTEM_IDS):
    if i in regset: fail(f"ecosystem-ids.txt: {i} is registered here too")
    if i not in page_ids: fail(f"ecosystem-ids.txt: {i} is on no page")

for f in fails: print("COVERAGE FAIL: " + f, file=sys.stderr)
eco = sum(1 for i in page_ids if i not in regset)
print(f"[coverage] {len(stdlib_pages)} stdlib class pages: {covered} with a bench page, {exempt} exempt; "
      f"{npages} bench pages, {nblocks} blocks, {len(page_ids)} page ids: {len(page_ids) - eco} registered "
      f"(of {len(reg)} registered ids), {eco} ecosystem")
if fails:
    print(f"COVERAGE FAILED ({len(fails)})"); sys.exit(1)
print("COVERAGE OK")
EOF
