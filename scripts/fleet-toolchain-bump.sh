#!/usr/bin/env bash
# Move dev.cajeta.* libraries to a cajeta release, testing each one the way its
# CI will before anything is pushed.
#
#   scripts/fleet-toolchain-bump.sh v0.51.0                 # the default repo list
#   scripts/fleet-toolchain-bump.sh v0.51.0 cajeta-http cajeta-codec
#
# For each repo it makes a worktree off origin/main in a directory of its own,
# so no sibling checkout can stand in for what CI fetches from Olla. It raises
# the workflow pins (CAJETA_VERSION / cajeta-version), every manifest's
# "toolchain" version and .cajeta-version, then runs the repo's CI test command
# on the release binary under an empty HOME, with the CI pre-steps that command
# depends on. A green repo gets a "toolchain: cajeta vX" commit in its worktree.
# Nothing is pushed: the summary prints each worktree and the push to run.
#
# Env:
#   CAJETA_FLEET_ROOT  where the repos are cloned (default: the parent of this checkout)
#   CAJETA_FLEET_WORK  scratch directory (default: ${TMPDIR:-/tmp}/cajeta-fleet-<version>)
#   CAJETA_BIN         a release cajeta binary to use instead of downloading <version>
#   CAJETA_LIB_PATH    prepended to LD_LIBRARY_PATH (the Linux release binary needs libzmq)
#   JOBS               repos tested at once (default 4; each takes a few GB)
#
# The pre-steps are the ones measured on 2026-11-03/04 (v0.50.0 and v0.51.0):
#   - lib-release.yml runs `cajeta build` before its test-command;
#   - cajeta-codec's CI checks out cajeta-unit v0.3.6 as UNIT_REPO, then runs
#     the tour (samples/tour/run-tour.sh);
#   - cajeta-cabra's CI checks out cajeta-llm v0.1.5, cajeta-codec v0.8.7 and
#     cajeta-jinja v0.1.6 beside it and builds codec's native zlib archive.
# When a workflow changes those, change them here too.
set -u

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ $# -ge 1 ] || usage
case "$1" in v[0-9]*.[0-9]*.[0-9]*) ;; *) usage ;; esac

V=$1; shift
VN=${V#v}
here="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${CAJETA_FLEET_ROOT:-$(dirname "$here")}"
WORK="${CAJETA_FLEET_WORK:-${TMPDIR:-/tmp}/cajeta-fleet-$VN}"
JOBS="${JOBS:-4}"

if [ $# -gt 0 ]; then
    REPOS=("$@")
else
    REPOS=(cajeta-cabra cajeta-primavera cajeta-unit cajeta-codec cajeta-crypto
           cajeta-jinja cajeta-logging cajeta-http cajeta-cloud cajeta-cloud-s3
           cajeta-cloud-cloudflare cajeta-cloud-aws cajeta-cloud-local)
fi

mkdir -p "$WORK"
if [ -z "${CAJETA_BIN:-}" ]; then
    tarball="cajeta-$V-x86_64-linux-gnu.tar.gz"
    if [ ! -x "$WORK/toolchain/cajeta-$V-x86_64-linux-gnu/bin/cajeta" ]; then
        mkdir -p "$WORK/toolchain"
        gh release download "$V" --repo jklappenbach/cajeta --pattern "$tarball" \
            --dir "$WORK/toolchain" --clobber || { echo "fleet: cannot download $tarball" >&2; exit 1; }
        tar -xzf "$WORK/toolchain/$tarball" -C "$WORK/toolchain"
    fi
    CAJETA_BIN="$WORK/toolchain/cajeta-$V-x86_64-linux-gnu/bin/cajeta"
fi
# The tests run under an empty HOME, which would hide the user's git identity
# from the commit; carry it across explicitly.
GIT_AUTHOR_NAME="${GIT_AUTHOR_NAME:-$(git config user.name)}"
GIT_AUTHOR_EMAIL="${GIT_AUTHOR_EMAIL:-$(git config user.email)}"
GIT_COMMITTER_NAME="${GIT_COMMITTER_NAME:-$GIT_AUTHOR_NAME}"
GIT_COMMITTER_EMAIL="${GIT_COMMITTER_EMAIL:-$GIT_AUTHOR_EMAIL}"
export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
export CAJETA_BIN V VN ROOT WORK
export CAJETA_LIB_PATH="${CAJETA_LIB_PATH:-}"
LD_LIBRARY_PATH="$CAJETA_LIB_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$CAJETA_BIN" --version \
    || { echo "fleet: $CAJETA_BIN does not run (set CAJETA_LIB_PATH to a directory with libzmq)" >&2; exit 1; }

one() {
    local r=$1 src="$ROOT/$1" D="$WORK/$1" W="$WORK/$1/$1" L="$WORK/$1.log"
    local pre="" cmd="" post="" rc=0
    rm -rf "$D"; mkdir -p "$D/home"
    if [ ! -d "$src/.git" ]; then echo ">> $r exit 1: no clone at $src" > "$L"; return; fi
    git -C "$src" worktree prune
    git -C "$src" fetch -q origin
    git -C "$src" worktree add -q --detach "$W" origin/main || { echo ">> $r exit 1: worktree" > "$L"; return; }
    (
        cd "$W" || exit 1
        sed -i -E "s/^(\s*(CAJETA_VERSION|cajeta-version):\s*)v[0-9]+\.[0-9]+\.[0-9]+/\1$V/" .github/workflows/*.yml
        [ -f .cajeta-version ] && echo "$V" > .cajeta-version
        git ls-files '*cajeta.json' | while read -r m; do
            python3 - "$m" "$VN" <<'PY'
import re, sys
p, vn = sys.argv[1], sys.argv[2]
s = open(p).read()
t = re.sub(r'("toolchain"\s*:\s*\{\s*"version"\s*:\s*")[0-9]+\.[0-9]+\.[0-9]+(")', r'\g<1>' + vn + r'\2', s)
if t != s:
    open(p, 'w').write(t)
PY
        done
    )
    if git -C "$W" diff --quiet; then
        echo ">> $r exit 0: already on $V, nothing to change" > "$L"
        return
    fi
    export HOME="$D/home" PATH="$(dirname "$CAJETA_BIN"):$PATH" CAJETA="$CAJETA_BIN"
    export LD_LIBRARY_PATH="$CAJETA_LIB_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    case $r in
        cajeta-codec)
            git -C "$ROOT/cajeta-unit" fetch -q --tags
            git -C "$ROOT/cajeta-unit" worktree add -q --detach "$D/unit-dep" v0.3.6
            export UNIT_REPO="$D/unit-dep"
            cmd="./run-tests.sh"; post="./samples/tour/run-tour.sh" ;;
        cajeta-cabra)
            local p q
            for p in cajeta-llm:v0.1.5 cajeta-codec:v0.8.7 cajeta-jinja:v0.1.6; do
                q=${p%%:*}
                git -C "$ROOT/$q" fetch -q --tags
                git -C "$ROOT/$q" worktree add -q --detach "$D/$q" "${p#*:}"
            done
            ( cd "$D/cajeta-codec" && ./native/build.sh ) > "$D/native.log" 2>&1
            export CAJETA_NATIVE_PATH="$D/cajeta-codec/native" LLM_REPO="$D/no-llm-build"
            cmd="./run-tests.sh" ;;
    esac
    if [ -z "$cmd" ]; then
        cmd=$(grep -hE '^\s*test-command:' "$W"/.github/workflows/*.yml | head -1 | sed -E 's/^\s*test-command:\s*//')
        [ -z "$cmd" ] && cmd=./run-tests.sh
        grep -q "lib-release.yml" "$W"/.github/workflows/*.yml && pre="cajeta build"
        [ -x "$W/run-tour.sh" ] && grep -q "run-tour.sh" "$W"/.github/workflows/*.yml && post="./run-tour.sh"
    fi
    {
        echo ">> $r start $(date +%H:%M) pre: ${pre:-none} cmd: $cmd post: ${post:-none}"
        git -C "$W" diff --stat
    } > "$L"
    cd "$W" || return
    [ -n "$pre" ] && { timeout 1800 bash -c "$pre" >> "$L" 2>&1 || rc=$?; }
    [ $rc -eq 0 ] && { timeout 3600 bash -c "$cmd" >> "$L" 2>&1 || rc=$?; }
    [ $rc -eq 0 ] && [ -n "$post" ] && { timeout 1800 bash -c "$post" >> "$L" 2>&1 || rc=$?; }
    if [ $rc -eq 0 ]; then
        local tot
        tot=$(grep -hE '^Tests:|=== [0-9]+ passed' "$L" | head -1 | sed -E 's/^(=== )?Tests: //; s/ ===$//; s/^=== //')
        git -C "$W" add -u
        git -C "$W" commit -q -m "toolchain: cajeta $V" \
            -m "The suite passed on $V locally under an empty HOME${tot:+ ($tot)}." >> "$L" 2>&1 \
            || { echo ">> $r: the tests passed but the commit failed" >> "$L"; rc=1; }
    fi
    echo ">> $r exit $rc end $(date +%H:%M)" >> "$L"
}
export -f one

echo ">> fleet: ${#REPOS[@]} repos to $V, $JOBS at a time, logs in $WORK (start $(date +%H:%M))"
printf '%s\n' "${REPOS[@]}" | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {}

echo
fails=0
for r in "${REPOS[@]}"; do
    last=$(tail -1 "$WORK/$r.log" 2>/dev/null)
    case "$last" in
        *"exit 0 end"*)
            echo "GREEN  $r  ($WORK/$r/$r)"
            echo "       git -C $WORK/$r/$r push origin HEAD:main" ;;
        *"already on"*) echo "SAME   $r" ;;
        *) echo "RED    $r  ${last:-no log}  -> $WORK/$r.log"; fails=$((fails + 1)) ;;
    esac
done
echo ">> fleet: done $(date +%H:%M), $fails red"
[ $fails -eq 0 ]
