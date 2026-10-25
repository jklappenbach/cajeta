#!/usr/bin/env bash
# benchmark-reference plan 2.1.2 — the catalog layout (spec §2.1). Static: no
# build, no run. Asserts:
#  (1) samples/profile is retired and the old flat bench/src/bench is gone;
#  (2) every .cajeta file under bench/src/main/cajeta/bench/ sits inside a
#      <package>/ directory, except the entry point Bench.cajeta; the only
#      non-catalog directory is selftest/ (the harness's own scaffold
#      benchmarks, registered only under CAJETA_BENCH_SCAFFOLD=1). The harness
#      is the dev.cajeta.bench library: no local copy, and cajeta.json
#      depends on it;
#  (1b) every catalog package directory has its <Package>Registry and
#      Bench.cajeta only calls the registries, in catalog order;
#  (3) every catalog benchmark's area() is its <package> (the directory it
#      lives in) and name() its <slug>, so the id <area>/<name> is unique, and
#      it is registered in its package's Registry;
#  (4) every <package> has a page directory bench/<package>/ and one of its
#      pages carries the benchmark's id (`id: <area>/<name>` or it in backticks) (skip with LAYOUT_PAGES=0 while the pages are written);
#  (5) the device build tasks are gpu-nvptx, gpu-vulkan and gpu-amdgpu, and
#      the retired amdgpu-only `gpu` task is gone;
#  (6) bench/registered-ids.txt (the site's registered-id hook, written from
#      `./bench.sh --list`) holds exactly the catalog's ids.
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
SRC="$PROJ/src/main/cajeta/bench"
ENTRY="$SRC/Bench.cajeta"

# spec §6: the language + stdlib catalog packages.
PACKAGES="lang collection concurrent time hash codec io math nucleo wire search process reflect aot xpu gfx toolchain"
NONCATALOG="selftest"

FAILS=0
fail() { echo "LAYOUT FAIL: $1" >&2; FAILS=$((FAILS+1)); }
is_in() { case " $2 " in *" $1 "*) return 0;; *) return 1;; esac; }
regname() { printf '%s%sRegistry' "$(printf '%s' "${1:0:1}" | tr a-z A-Z)" "${1:1}"; }

# (1) retired locations
[[ -e "$REPO_ROOT/samples/profile" ]] && fail "samples/profile still exists (moved to bench/)"
[[ -e "$PROJ/src/bench" ]] && fail "old flat bench/src/bench still exists"
[[ -e "$PROJ/kernel-startup" ]] && fail "bench/kernel-startup still exists (moved under bench/toolchain/)"
[[ -e "$PROJ/report/report.py" ]] && fail "report/report.py still exists (the site renders results)"

[[ -d "$SRC" ]] || { fail "no harness project at bench/src/main/cajeta/bench/"; echo "LAYOUT FAILED ($FAILS)"; exit 1; }
[[ -f "$ENTRY" ]] || fail "no entry point bench/src/main/cajeta/bench/Bench.cajeta"
[[ -e "$SRC/harness" ]] && fail "a local harness copy exists (bench/src/main/cajeta/bench/harness/); the harness is dev.cajeta.bench"
grep -q '"dev.cajeta.bench"' "$PROJ/cajeta.json" || fail "cajeta.json does not depend on dev.cajeta.bench"
grep -q 'BenchMain.run(args' "$ENTRY" 2>/dev/null || fail "Bench.cajeta does not hand off to dev.cajeta.bench's BenchMain.run"

# (1b) per-package registration: every catalog package (and selftest/) has a
# <Package>Registry with register(ArrayList<Benchmark> b); Bench.cajeta adds
# no benchmark itself and calls exactly selftest's then the seventeen
# packages' registries, in catalog order.
WANT_CALLS=""
for pkg in selftest $PACKAGES; do
    r="$(regname "$pkg")"
    f="$SRC/$pkg/$r.cajeta"
    [[ -f "$f" ]] || { fail "no $pkg/$r.cajeta"; continue; }
    grep -q "^package bench\.${pkg};" "$f" || fail "$pkg/$r.cajeta is not in package bench.$pkg"
    grep -q "public static void register(ArrayList<Benchmark> b)" "$f" || fail "$pkg/$r.cajeta has no register(ArrayList<Benchmark> b)"
    WANT_CALLS="$WANT_CALLS$r.register(benchmarks);"
done
if [[ -f "$ENTRY" ]]; then
    grep -qE '\.add\(|heap [A-Za-z0-9_]+\(' "$ENTRY" && grep -E '^\s+[^/ ].*(\.add\(|heap [A-Za-z0-9_]+\()' "$ENTRY" | grep -v 'heap ArrayList<Benchmark>()' | grep -q . \
        && fail "Bench.cajeta registers a benchmark directly (only the registries may)"
    GOT_CALLS="$(grep -oE '[A-Za-z]+Registry\.register\(benchmarks\);' "$ENTRY" | tr -d '\n')"
    [[ "$GOT_CALLS" == "$WANT_CALLS" ]] || fail "Bench.cajeta registry calls are not selftest + the seventeen packages in catalog order: [$GOT_CALLS]"
fi

NB=0
IDS=""
# (2) every file inside a package directory; (3) and (4) for every benchmark
# class. A benchmark class is one that extends Benchmark, ExternalBenchmark or
# ServiceBenchmark, directly or through a class in this tree (XpuBench,
# ToolchainExternal); a class another benchmark class extends is a base and is
# not itself registered. A class may be constructed with arguments and name
# several ids (ChannelBench(1, 1) is channel-spsc): every literal its name()
# returns is an id. Prints "FAIL <msg>" lines, then "ID <id>" lines, then
# "NB <count>".
SCAN="$(PACKAGES="$PACKAGES" NONCATALOG="$NONCATALOG" LAYOUT_PAGES="${LAYOUT_PAGES:-1}" python3 - "$SRC" "$PROJ" <<'EOF'
import os, re, sys
src, proj = sys.argv[1], sys.argv[2]
packages = os.environ["PACKAGES"].split()
noncat = os.environ["NONCATALOG"].split()
pages_on = os.environ["LAYOUT_PAGES"] != "0"
ROOTS = {"Benchmark", "ExternalBenchmark", "ServiceBenchmark"}
fails, files = [], []
for d, _, fs in os.walk(src):
    for f in fs:
        if f.endswith(".cajeta"):
            files.append(os.path.join(d, f))
files.sort()
cls = {}   # "pkg.Name" -> (rel, pkg, parent key or root name, text)
for f in files:
    rel = os.path.relpath(f, src)
    if "/" not in rel:
        if rel != "Bench.cajeta":
            fails.append(f"{rel} is outside a <package>/ directory")
        continue
    pkg = rel.split("/")[0]
    if pkg in noncat:
        continue
    if pkg not in packages:
        fails.append(f"{rel}: '{pkg}' is not a catalog package ({' '.join(packages)})")
        continue
    text = open(f, encoding="utf-8").read()
    for m in re.finditer(r"^public\s+(?:final\s+)?class\s+(\w+)(?:<[^>]*>)?(?:\s+extends\s+(\w+))?", text, re.M):
        cls[pkg + "." + m.group(1)] = (rel, pkg, m.group(2), text)
# A parent resolves in the class's own package first (two packages each have a
# SaxpyBench), then in any package; a root stays its bare name.
def resolve(pkg, name):
    if name is None or name in ROOTS: return name
    if pkg + "." + name in cls: return pkg + "." + name
    for k in cls:
        if k.endswith("." + name): return k
    return name
cls = {k: (r, p, resolve(p, par), t) for k, (r, p, par, t) in cls.items()}
def is_bench(c, seen=()):
    p = cls[c][2]
    if p in ROOTS: return True
    return p in cls and p not in seen and is_bench(p, seen + (c,))
bench = [c for c in cls if is_bench(c)]
bases = {cls[c][2] for c in bench if cls[c][2] in cls}
def method_body(text, sig):
    i = text.find(sig)
    if i < 0: return None
    j = text.find("{", i); depth = 0
    for k in range(j, len(text)):
        if text[k] == "{": depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0: return text[j:k]
    return None
def literals(body):
    return re.findall(r'return\s+"([^"]*)"\s*;', body or "")
def inherited(c, sig):
    while c in cls:
        b = method_body(cls[c][3], sig)
        if b is not None: return b
        c = cls[c][2]
    return None
ids, nb = [], 0
for c in sorted(bench, key=lambda c: cls[c][0]):
    if c in bases: continue
    rel, pkg, parent, text = cls[c]
    if parent == "Benchmark" and not re.search(r"^import dev\.cajeta\.bench\.Benchmark;", text, re.M):
        fails.append(f"{rel}: Benchmark is not dev.cajeta.bench.Benchmark")
    nb += 1
    areas = literals(inherited(c, "String area()"))
    names = literals(method_body(text, "String name()") if "String name()" in text else inherited(c, "String name()"))
    for dn, da in re.findall(r'this\.describe\("([^"]+)",\s*"([^"]+)"\)', text):   # ExternalBenchmark
        names = names or [dn]; areas = areas or [da]
    if not areas: fails.append(f"{rel}: no literal area()"); continue
    if not names: fails.append(f"{rel}: no literal name()"); continue
    if any(a != pkg for a in areas):
        fails.append(f"{rel}: area() {areas} != its package '{pkg}' (area is the package only)"); continue
    reg = os.path.join(src, pkg, pkg[0].upper() + pkg[1:] + "Registry.cajeta")
    regtext = open(reg, encoding="utf-8").read() if os.path.exists(reg) else ""
    if not re.search(r"^\s*b\.add\(heap " + c.split(".")[1] + r"\(", regtext, re.M):
        fails.append(f"{rel}: not registered in {pkg}/{os.path.basename(reg)}")
    for n in dict.fromkeys(names):
        if not re.match(r"^[a-z0-9][a-z0-9_.-]*$", n):
            fails.append(f"{rel}: name() '{n}' is not a lower-kebab slug")
        i = f"{pkg}/{n}"
        if i in ids: fails.append(f"{rel}: duplicate id {i}")
        ids.append(i)
        if pages_on:
            pd = os.path.join(proj, pkg)
            mds = [os.path.join(d, f) for d, _, fs in os.walk(pd) for f in fs if f.endswith(".md")] if os.path.isdir(pd) else []
            if not mds:
                fails.append(f"{i}: no pages under bench/{pkg}/")
            elif not any(("id: " + i) in open(m, encoding="utf-8").read() or ("`" + i + "`") in open(m, encoding="utf-8").read() for m in mds):
                fails.append(f"{i}: no page under bench/{pkg}/ carries the id '{i}'")
for f in fails: print("FAIL " + f)
for i in ids: print("ID " + i)
print(f"NB {nb}")
EOF
)"
while IFS= read -r line; do
    case "$line" in
        "FAIL "*) fail "${line#FAIL }" ;;
        "ID "*) IDS="$IDS ${line#ID }" ;;
        "NB "*) NB="${line#NB }" ;;
    esac
done <<< "$SCAN"

[[ "$NB" -gt 0 ]] || fail "no catalog benchmarks found"

# (5) the device builds: one task per GPU backend, each with cpu beside it;
# the retired amdgpu-only `gpu` task is gone (gpu-amdgpu replaces it).
for t in gpu-nvptx gpu-vulkan gpu-amdgpu; do
    grep -q "^        \"$t\": {" "$PROJ/cajeta.json" || fail "cajeta.json has no $t task"
done
grep -q '^        "gpu": {' "$PROJ/cajeta.json" && fail "cajeta.json still has the retired gpu task (gpu-amdgpu replaces it)"
grep -q '"xpu-backend": "amdgpu,cpu", "xpu-arch": "gfx1151"' "$PROJ/cajeta.json" || fail "gpu-amdgpu is not amdgpu,cpu on gfx1151"

# (6) the registered-id list (bench/registered-ids.txt, the site's hook) is
# `bench.sh --list` as built: every catalog id above, no other, in its order.
REGF="$PROJ/registered-ids.txt"
if [[ ! -f "$REGF" ]]; then
    fail "no bench/registered-ids.txt (write it from ./bench.sh --list)"
else
    GOT="$(grep -vE '^\s*(#|$)' "$REGF" | awk '{print $1}' | sort)"
    WANT="$(printf '%s\n' $IDS | sort)"
    [[ "$GOT" == "$WANT" ]] || fail "registered-ids.txt differs from the catalog's ids: $(diff <(printf '%s\n' "$WANT") <(printf '%s\n' "$GOT") | grep '^[<>]' | head -40 | tr '\n' ' ')"
    DUP="$(grep -vE '^\s*(#|$)' "$REGF" | awk '{print $1}' | sort | uniq -d | head -3 | tr '\n' ' ')"
    [[ -z "$DUP" ]] || fail "registered-ids.txt repeats ids: $DUP"
fi

if [[ "$FAILS" -gt 0 ]]; then
    echo "LAYOUT FAILED ($FAILS problems, $NB benchmarks)"
    exit 1
fi
echo "[layout] $NB catalog benchmarks, every one under its <package>/ with id <area>/<name> on a page"
echo "LAYOUT OK"
