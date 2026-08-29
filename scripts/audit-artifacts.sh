#!/usr/bin/env bash
# Audit every published cajeta repo for generated files that should never be
# tracked, and for the .gitignore entries that keep them out.
#
# build-output-layout plan 1.2.4. Written 2026-08-27 after a compile handed
# two source roots bound the SECOND to the output directory and wrote object
# files into it at exit 0 — 180 objects into cajeta-cabra/src, 75 into
# cajeta-llm/src, both then swept into git by a routine `git add -A`. The
# arity is rejected now and the compiler refuses to write into a source tree,
# but "no repo is carrying generated files" is a claim that should be
# MEASURED on demand rather than remembered.
#
# Reads GitHub over the API — no cloning, so it is cheap enough to run often.
#
# Usage:  scripts/audit-artifacts.sh [owner]      (default: jklappenbach)
# Exit:   0 = every repo clean; 1 = at least one finding.
set -uo pipefail

OWNER="${1:-jklappenbach}"

# Extensions that are always generated. `.cja` counts: an archive is build
# output, and the cache under .cajeta/ is full of them.
ART_RE='\.(o|obj|a|so|dylib|cja|lib|pdb|exe)$'
# Paths that are always generated regardless of extension.
PATH_RE='^(build/|\.cajeta/cache/)'

fail=0
printf "%-22s %10s %10s %9s %s\n" "REPO" "ARTIFACTS" "CACHE" "BUILD_IGN" "VERDICT"

repos=$(gh repo list "$OWNER" --limit 200 --json name,isFork \
        --jq '.[] | select(.isFork|not) | .name' | grep -E '^cajeta' | sort)

for r in $repos; do
    br=$(gh repo view "$OWNER/$r" --json defaultBranchRef \
         --jq '.defaultBranchRef.name' 2>/dev/null)
    if [ -z "$br" ] || [ "$br" = "null" ]; then
        printf "%-22s %10s %10s %9s %s\n" "$r" "-" "-" "-" "empty repo"
        continue
    fi

    paths=$(gh api "repos/$OWNER/$r/git/trees/$br?recursive=1" \
            --jq '.tree[]|select(.type=="blob")|.path' 2>/dev/null)
    n_art=$(printf '%s\n' "$paths" | grep -cE "$ART_RE")
    n_gen=$(printf '%s\n' "$paths" | grep -cE "$PATH_RE")

    gi=$(gh api "repos/$OWNER/$r/contents/.gitignore" --jq '.content' 2>/dev/null \
         | base64 -d 2>/dev/null)
    if [ -z "$gi" ]; then
        bi="NO-FILE"
    elif printf '%s\n' "$gi" | grep -qE '^[[:space:]]*/?build/?[[:space:]]*$'; then
        bi="yes"
    else
        bi="NO"
    fi

    verdict="clean"
    if [ "$n_art" -gt 0 ] || [ "$n_gen" -gt 0 ] || [ "$bi" != "yes" ]; then
        verdict="FINDING"; fail=1
    fi
    printf "%-22s %10s %10s %9s %s\n" "$r" "$n_art" "$n_gen" "$bi" "$verdict"
done

echo
if [ "$fail" -eq 0 ]; then
    echo "audit: clean — no tracked build output, build/ ignored everywhere"
else
    echo "audit: findings above. Untracking is safe and unilateral; purging"
    echo "       HISTORY is not — these are public repos whose main other"
    echo "       clones have pulled, so that is a per-repo decision."
fi
exit "$fail"
