#!/usr/bin/env bash
# benchmark-reference plan 2.1.2 — the catalog layout (spec §2.1). Static: no
# build, no run. Asserts:
#  (1) samples/profile is retired and the old flat bench/src/bench is gone;
#  (2) every .cajeta file under bench/src/main/cajeta/bench/ sits inside a
#      <package>/ directory, except the entry point Bench.cajeta; the only
#      non-catalog directories are selftest/ (the harness's own scaffold
#      benchmarks, registered only under CAJETA_BENCH_SCAFFOLD=1) and harness/
#      (the local harness, until the project depends on dev.cajeta.bench);
#  (3) every catalog benchmark's area() is "<package>/<slug>", <package> being
#      the directory it lives in, and it is registered in Bench.cajeta;
#  (4) every <package> has a page directory bench/<package>/ and one of its
#      pages carries the benchmark's id (`id: <area>` or `<area>` in backticks) (skip with LAYOUT_PAGES=0 while the pages are written).
set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${PROJ}/.." &> /dev/null && pwd )"
SRC="$PROJ/src/main/cajeta/bench"
ENTRY="$SRC/Bench.cajeta"

# spec §6: the language + stdlib catalog packages.
PACKAGES="lang collection concurrent time hash codec io math nucleo wire search process reflect xpu toolchain"
NONCATALOG="selftest harness"

FAILS=0
fail() { echo "LAYOUT FAIL: $1" >&2; FAILS=$((FAILS+1)); }
is_in() { case " $2 " in *" $1 "*) return 0;; *) return 1;; esac; }

# (1) retired locations
[[ -e "$REPO_ROOT/samples/profile" ]] && fail "samples/profile still exists (moved to bench/)"
[[ -e "$PROJ/src/bench" ]] && fail "old flat bench/src/bench still exists"
[[ -e "$PROJ/kernel-startup" ]] && fail "bench/kernel-startup still exists (moved under bench/toolchain/)"
[[ -e "$PROJ/report/report.py" ]] && fail "report/report.py still exists (the site renders results)"

[[ -d "$SRC" ]] || { fail "no harness project at bench/src/main/cajeta/bench/"; echo "LAYOUT FAILED ($FAILS)"; exit 1; }
[[ -f "$ENTRY" ]] || fail "no entry point bench/src/main/cajeta/bench/Bench.cajeta"

# (2) every file inside a package directory
NB=0
while IFS= read -r f; do
    rel="${f#$SRC/}"
    if [[ "$rel" != */* ]]; then
        [[ "$rel" == "Bench.cajeta" ]] || fail "$rel is outside a <package>/ directory"
        continue
    fi
    pkg="${rel%%/*}"
    if is_in "$pkg" "$NONCATALOG"; then continue; fi
    is_in "$pkg" "$PACKAGES" || { fail "$rel: '$pkg' is not a catalog package ($PACKAGES)"; continue; }

    # (3) area() = "<package>/<slug>" for every Benchmark subclass
    grep -q 'extends Benchmark' "$f" || continue
    NB=$((NB+1))
    cls="$(basename "$f" .cajeta)"
    area="$(grep -A2 'String area()' "$f" | grep -oE 'return "[^"]*"' | head -1 | sed 's/return "//; s/"$//')"
    [[ -n "$area" ]] || { fail "$rel: no literal area()"; continue; }
    apkg="${area%%/*}"; slug="${area#*/}"
    [[ "$area" == */* ]] || { fail "$rel: area() '$area' is not <package>/<slug>"; continue; }
    [[ "$apkg" == "$pkg" ]] || fail "$rel: area() package '$apkg' != directory '$pkg'"
    [[ "$slug" =~ ^[a-z0-9][a-z0-9_.-]*$ ]] || fail "$rel: slug '$slug' is not lower-kebab"
    grep -q "^import bench\.${pkg}\.${cls};" "$ENTRY" 2>/dev/null || fail "$rel: not imported in Bench.cajeta"
    grep -q "heap ${cls}()" "$ENTRY" 2>/dev/null || fail "$rel: not registered in Bench.cajeta"

    # (4) the page directory mentions the slug
    if [[ "${LAYOUT_PAGES:-1}" != "0" ]]; then
        if [[ ! -d "$PROJ/$pkg" ]] || ! ls "$PROJ/$pkg"/*.md >/dev/null 2>&1; then
            fail "$area: no pages under bench/$pkg/"
        elif ! grep -rqF --include="*.md" -e "id: $area" -e "\`$area\`" "$PROJ/$pkg"; then
            fail "$area: no page under bench/$pkg/ carries the id '$area'"
        fi
    fi
done < <(find "$SRC" -name '*.cajeta' | sort)

[[ "$NB" -gt 0 ]] || fail "no catalog benchmarks found"

if [[ "$FAILS" -gt 0 ]]; then
    echo "LAYOUT FAILED ($FAILS problems, $NB benchmarks)"
    exit 1
fi
echo "[layout] $NB catalog benchmarks, every one under its <package>/ with area() = <package>/<slug>"
echo "LAYOUT OK"
