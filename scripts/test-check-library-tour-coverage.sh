#!/usr/bin/env bash
# Fixture tests for check-library-tour-coverage.sh (tour-quality plan 1.1).
#
# Builds a throwaway library (two classes + one annotation) and four tour
# variants, then asserts the checker's exit codes and its naming of uncovered
# types:
#   partial tour    -> exit 1, names demo.lib.Beta and demo.lib.Gamma
#   full tour       -> exit 0
#   wildcard tour   -> exit 0 (import demo.lib.* covers the package)
#   annotation tour -> exit 0 for Gamma via `@Gamma` usage without an import
#                      (annotations resolve without import lines)
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CHECK="$REPO_ROOT/scripts/check-library-tour-coverage.sh"
export CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"

[ -x "$CAJETA" ] || { echo "compiler not found: $CAJETA" >&2; exit 2; }
[ -e "$CHECK" ] || { echo "FAIL: checker missing: $CHECK" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

LIB="$WORK/lib/src/main/cajeta"
mkdir -p "$LIB/demo/lib"
cat > "$LIB/demo/lib/Alpha.cajeta" <<'EOF'
package demo.lib;

/** Fixture class the partial tour imports. */
public final class Alpha {
    public Alpha() {
    }

    public int64 value() {
        return 1;
    }
}
EOF
cat > "$LIB/demo/lib/Beta.cajeta" <<'EOF'
package demo.lib;

/** Fixture class the partial tour misses. */
public final class Beta {
    public Beta() {
    }

    public int64 value() {
        return 2;
    }
}
EOF
cat > "$LIB/demo/lib/Gamma_annotation.cajeta" <<'EOF'
package demo.lib;

/** Fixture annotation — used via @Gamma, never imported. */
public annotation Gamma { }
EOF

mk_tour() {  # $1 = dir, remaining args = import lines
    local dir="$1"; shift
    mkdir -p "$dir/src/tour"
    { echo "package tour;"; echo
      for imp in "$@"; do echo "import $imp;"; done
    } > "$dir/src/tour/Tour.cajeta"
}
mk_tour "$WORK/tour-partial"  "demo.lib.Alpha"
mk_tour "$WORK/tour-full"     "demo.lib.Alpha" "demo.lib.Beta"
mk_tour "$WORK/tour-wildcard" "demo.lib.*"
mk_tour "$WORK/tour-anno"     "demo.lib.Alpha" "demo.lib.Beta"
cat >> "$WORK/tour-anno/src/tour/Tour.cajeta" <<'EOF'

@Gamma
public class Annotated {
}
EOF

fails=0
expect() {  # $1 = name, $2 = expected exit, $3 = tour dir, $4 = required output regex ('' = none)
    local name="$1" want="$2" tour="$3" pattern="$4" out rc
    out="$("$CHECK" "$LIB" "$tour" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want" ]; then
        echo "FAIL: $name — exit $rc, wanted $want"; echo "$out" | sed 's/^/    /'
        fails=$((fails + 1)); return
    fi
    if [ -n "$pattern" ] && ! grep -q "$pattern" <<< "$out"; then
        echo "FAIL: $name — output lacks /$pattern/"; echo "$out" | sed 's/^/    /'
        fails=$((fails + 1)); return
    fi
    echo "ok: $name"
}

expect "partial tour flags Beta"      1 "$WORK/tour-partial"  'demo\.lib\.Beta'
expect "partial tour flags Gamma"     1 "$WORK/tour-partial"  'demo\.lib\.Gamma'
expect "full tour flags unused anno"  1 "$WORK/tour-full"     'demo\.lib\.Gamma'
expect "wildcard import covers pkg"   0 "$WORK/tour-wildcard" ''
expect "@Gamma usage covers Gamma"    0 "$WORK/tour-anno"     ''

if [ "$fails" -ne 0 ]; then
    echo "test-check-library-tour-coverage: $fails failure(s)"; exit 1
fi
echo "test-check-library-tour-coverage: all green"
