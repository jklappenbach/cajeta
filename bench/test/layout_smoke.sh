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
#      pages carries the benchmark's id (`id: <area>/<name>` or it in backticks) (skip with LAYOUT_PAGES=0 while the pages are written).
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

# (2) every file inside a package directory
NB=0
IDS=""
while IFS= read -r f; do
    rel="${f#$SRC/}"
    if [[ "$rel" != */* ]]; then
        [[ "$rel" == "Bench.cajeta" ]] || fail "$rel is outside a <package>/ directory"
        continue
    fi
    pkg="${rel%%/*}"
    if is_in "$pkg" "$NONCATALOG"; then continue; fi
    is_in "$pkg" "$PACKAGES" || { fail "$rel: '$pkg' is not a catalog package ($PACKAGES)"; continue; }

    # (3) area() = "<package>" and name() = "<slug>": the id is <area>/<name>
    grep -q 'extends Benchmark' "$f" || continue
    grep -q '^import dev\.cajeta\.bench\.Benchmark;' "$f" || fail "$rel: Benchmark is not dev.cajeta.bench.Benchmark"
    NB=$((NB+1))
    cls="$(basename "$f" .cajeta)"
    area="$(grep -A2 'String area()' "$f" | grep -oE 'return "[^"]*"' | head -1 | sed 's/return "//; s/"$//')"
    name="$(grep -A2 'String name()' "$f" | grep -oE 'return "[^"]*"' | head -1 | sed 's/return "//; s/"$//')"
    [[ -n "$area" ]] || { fail "$rel: no literal area()"; continue; }
    [[ -n "$name" ]] || { fail "$rel: no literal name()"; continue; }
    [[ "$area" == "$pkg" ]] || { fail "$rel: area() '$area' != its package '$pkg' (area is the package only)"; continue; }
    [[ "$name" =~ ^[a-z0-9][a-z0-9_.-]*$ ]] || fail "$rel: name() '$name' is not a lower-kebab slug"
    id="$area/$name"
    if is_in "$id" "$IDS"; then fail "$rel: duplicate id $id"; fi
    IDS="$IDS $id"
    REG="$SRC/$pkg/$(regname "$pkg").cajeta"
    grep -q "^ *b\.add(heap ${cls}());" "$REG" 2>/dev/null || fail "$rel: not registered in $pkg/$(regname "$pkg").cajeta"

    # (4) the page directory mentions the slug
    if [[ "${LAYOUT_PAGES:-1}" != "0" ]]; then
        if [[ ! -d "$PROJ/$pkg" ]] || ! ls "$PROJ/$pkg"/*.md >/dev/null 2>&1; then
            fail "$id: no pages under bench/$pkg/"
        elif ! grep -rqF --include="*.md" -e "id: $id" -e "\`$id\`" "$PROJ/$pkg"; then
            fail "$id: no page under bench/$pkg/ carries the id '$id'"
        fi
    fi
done < <(find "$SRC" -name '*.cajeta' | sort)

[[ "$NB" -gt 0 ]] || fail "no catalog benchmarks found"

if [[ "$FAILS" -gt 0 ]]; then
    echo "LAYOUT FAILED ($FAILS problems, $NB benchmarks)"
    exit 1
fi
echo "[layout] $NB catalog benchmarks, every one under its <package>/ with id <area>/<name> on a page"
echo "LAYOUT OK"
