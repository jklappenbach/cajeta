#!/usr/bin/env bash
# Clone-on-demand vendoring for benchmark suites / competitor libraries.
# Reproducible: after a shallow clone of the default branch, the resolved commit
# is recorded in <name>.lock; re-running re-checks out that exact commit. Heavy
# suites are fetched only when an area unit needs them, not all up front.
#
# Usage:
#   vendor.sh --list            list the suites in the manifest
#   vendor.sh <name> [<name>..] vendor those suites into competitors/vendor/<name>/
#   vendor.sh --all             vendor everything (large; network-heavy)
set -euo pipefail

DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
MANIFEST="${DIR}/manifest.tsv"

fail() { echo "vendor: $1" >&2; exit 1; }
[[ -f "$MANIFEST" ]] || fail "manifest not found: $MANIFEST"

names() { awk -F'\t' '!/^#/ && NF>=2 {print $1}' "$MANIFEST"; }
url_of() { awk -F'\t' -v n="$1" '!/^#/ && $1==n {print $2; exit}' "$MANIFEST"; }

if [[ "${1:-}" == "--list" || $# -eq 0 ]]; then
    echo "Vendorable suites (competitors/vendor/):"
    while IFS=$'\t' read -r n u; do
        [[ -z "${n:-}" || "$n" == \#* ]] && continue
        lock="${DIR}/${n}.lock"
        state="not vendored"
        [[ -d "${DIR}/${n}" ]] && state="vendored$( [[ -f "$lock" ]] && echo " @ $(cat "$lock")" )"
        printf '  %-22s %-50s [%s]\n' "$n" "$u" "$state"
    done < "$MANIFEST"
    exit 0
fi

targets=()
if [[ "${1:-}" == "--all" ]]; then
    mapfile -t targets < <(names)
else
    targets=("$@")
fi

command -v git >/dev/null || fail "git not found"

for name in "${targets[@]}"; do
    url="$(url_of "$name")"
    [[ -n "$url" ]] || fail "unknown suite: $name"
    dest="${DIR}/${name}"
    lock="${DIR}/${name}.lock"
    if [[ -d "$dest/.git" ]]; then
        echo "[have]   $name (already cloned)"
    else
        echo "[clone]  $name <- $url"
        git clone --depth 1 "$url" "$dest" || fail "clone failed: $name"
    fi
    commit="$(git -C "$dest" rev-parse HEAD)"
    echo "$commit" > "$lock"
    echo "[lock]   $name @ $commit"
done
