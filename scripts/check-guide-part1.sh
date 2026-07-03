#!/usr/bin/env bash
# Transcript check for guide Part I (docs-refactor plan 7.1.1).
# Replays every command the chapters show against the current binary;
# fails if reality diverges from what the chapters claim.
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"
[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0
chk() { # <desc> <cmd...>
    local d="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "ok:   $d"; else echo "FAIL: $d"; fails=$((fails+1)); fi
}
chkgrep() { # <desc> <pattern> <cmd...>
    local d="$1" p="$2"; shift 2
    if "$@" 2>&1 | grep -qE "$p"; then echo "ok:   $d"; else echo "FAIL: $d"; fails=$((fails+1)); fi
}

# ch01/02 — install verification + command surface
chkgrep "cajeta --version prints version"      '^cajeta [0-9]'          "$CAJETA" --version
chkgrep "init --list shows the 5 archetypes"   'basic'                  "$CAJETA" init --list
chkgrep "init --list shows library"            'library'                "$CAJETA" init --list
chkgrep "init --list shows workspace"          'workspace'              "$CAJETA" init --list
chkgrep "init --list shows multi-binary"       'multi-binary'           "$CAJETA" init --list
chkgrep "init --list shows melt"               'melt'                   "$CAJETA" init --list
chk     "cajeta doc --help works"                                       "$CAJETA" doc --help

# ch03 — first project: binary
cd "$TMP"
chk     "init basic hello"                                              "$CAJETA" init basic hello
cd hello
chkgrep "tasks lists build"                    '^  build'               "$CAJETA" tasks
chkgrep "tasks lists clean"                    '^  clean'               "$CAJETA" tasks
chk     "cajeta build (binary)"                                         "$CAJETA" build
chk     "binary exists"                        test -x build/exe/com.example.basic
chkgrep "binary runs and greets"               'hello from'             ./build/exe/com.example.basic
chk     "cajeta clean"                                                  "$CAJETA" clean

# ch03 — library project
cd "$TMP"
chk     "init library greetlib"                                         "$CAJETA" init library greetlib
cd greetlib
chk     "cajeta build (library)"                                        "$CAJETA" build
chk     ".cja archive produced"                bash -c 'ls build/archive/*.cja'

# ch04 — running: no run task in basic archetype (chapters teach direct
# execution + adding a run task); verify the documented added-task flow
cd "$TMP/hello"
chk     "run task absent from archetype"       bash -c "! $CAJETA tasks | grep -qE '^  run'"

# ch05 — debugging entry points exist
chkgrep "dap subcommand advertised"            'dap'                    "$CAJETA" --help
chkgrep "ide subcommand advertised"            'ide'                    "$CAJETA" --help

# ch02 — the build-tool family is discoverable from --help (unit 8)
chkgrep "help lists init"                      '^  init'                "$CAJETA" --help
chkgrep "help lists build"                     '^  build'               "$CAJETA" --help
chkgrep "help lists test"                      '^  test'                "$CAJETA" --help
chkgrep "help lists tasks"                     '^  tasks'               "$CAJETA" --help
chkgrep "help lists install/publish"           '^  install|^  publish'  "$CAJETA" --help
chkgrep "help lists dependency mgmt"           'add.*remove.*upgrade|^  add' "$CAJETA" --help
chkgrep "help lists doc"                       '^  doc'                 "$CAJETA" --help
chkgrep "help mentions task fallthrough"       'manifest task|cajeta\.json task' "$CAJETA" --help

if [ "$fails" -gt 0 ]; then echo "check-guide-part1: $fails failure(s)"; exit 1; fi
echo "check-guide-part1: OK"
