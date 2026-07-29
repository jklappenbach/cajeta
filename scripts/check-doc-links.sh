#!/usr/bin/env bash
# Relative-link checker for markdown (docs-refactor plan 2.2.1).
#
# Usage: check-doc-links.sh [path ...]
#   paths: .md files or directories (searched recursively). Default:
#   docs/ specs/ README.md samples/tour/README.md at the repo root.
#
# Checks every [text](target) and ![alt](target) whose target is a
# relative path (http/https/mailto and pure #anchors are skipped;
# #fragments on files are stripped before the existence check).
# Exit 0 = all targets exist; 1 = broken links (listed on stdout).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"

roots=("$@")
if [ ${#roots[@]} -eq 0 ]; then
    cd "$REPO_ROOT"
    roots=(docs specs README.md samples/tour/README.md)
fi

collect_files() {
    local r
    for r in "${roots[@]}"; do
        if [ -f "$r" ]; then
            echo "$r"
        elif [ -d "$r" ]; then
            find "$r" -type f -name '*.md'
        fi
    done
}

broken=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    dir="$(dirname "$f")"
    # one link target per line; fenced code blocks excluded (indexing-
    # plus-call code like `arr[i](x)` would false-positive as a link)
    while IFS= read -r t; do
        [ -z "$t" ] && continue
        case "$t" in
            http://*|https://*|mailto:*|'#'*) continue ;;
        esac
        tgt="${t%%#*}"          # strip fragment
        tgt="${tgt%% \"*}"      # strip optional "title"
        tgt="${tgt%% }"
        [ -z "$tgt" ] && continue
        # only path-shaped targets (contain / or .); bare words are
        # almost always code that slipped through, not links
        case "$tgt" in
            */*|*.*) ;;
            *) continue ;;
        esac
        if [ ! -e "$dir/$tgt" ]; then
            echo "BROKEN: $f -> $t"
            broken=$((broken + 1))
        fi
    done < <(awk '/^```/ { fence = !fence; next } !fence' "$f" 2>/dev/null \
             | grep -oE '\]\([^)]+\)' | sed 's/^](//; s/)$//')
done < <(collect_files)

if [ "$broken" -gt 0 ]; then
    echo "check-doc-links: $broken broken link(s)"
    exit 1
fi
echo "check-doc-links: OK"
exit 0
