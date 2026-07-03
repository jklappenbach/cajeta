#!/usr/bin/env bash
# ```cajeta snippet compile check (docs-refactor plan 2.2.2).
#
# Extracts every ```cajeta fenced block from the given markdown files
# / directories and compiles it with the real compiler. A block is
# skipped when the line `<!-- snippet: skip -->` appears within the
# two lines above its fence (for intentionally partial fragments).
#
# Wrapping heuristic per block:
#   - has a `package` line            -> compiled verbatim
#   - has a top-level type decl       -> prepended `package snip;`
#   - method declaration(s)           -> wrapped in a host class
#   - otherwise (statement fragment)  -> wrapped in a class + method
# All of one file's unwrapped blocks share package `snip`, so later
# blocks can use types earlier blocks define (tutorial style).
#
# All of one file's blocks compile together in one synthetic source
# root (each block in its own package), via `cajeta jit-run` against
# a trivial driver entry. Compiler: $CAJETA or build/src/cajeta.
#
# Usage: check-doc-snippets.sh [path ...]
#   Default roots: docs/guide docs/stdlib README.md (the teaching
#   surfaces; pass paths explicitly for anything else).
# Exit 0 = every checked block compiles; 1 = failures (listed).
set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &>/dev/null && pwd)"
CAJETA="${CAJETA:-$REPO_ROOT/build/src/cajeta}"

if [ ! -x "$CAJETA" ]; then
    echo "check-doc-snippets: compiler not found at $CAJETA" >&2
    exit 2
fi

roots=("$@")
if [ ${#roots[@]} -eq 0 ]; then
    cd "$REPO_ROOT"
    roots=(docs/guide docs/stdlib README.md)
fi

collect_files() {
    local r
    for r in "${roots[@]}"; do
        if [ -f "$r" ]; then
            echo "$r"
        elif [ -d "$r" ]; then
            find "$r" -type f -name '*.md' -not -path '*/drafts/*'
        fi
    done
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

failures=0
checked=0

check_file() { # <md-file>
    local f="$1"
    local srcroot="$TMP/$(echo "$f" | tr '/.' '__')"
    local n=0 wrote=0

    # Extract blocks with awk: emits each block to $srcroot/blockN.raw,
    # honoring the skip marker.
    mkdir -p "$srcroot"
    awk -v out="$srcroot" '
        { hist2 = hist1; hist1 = $0 }
        /^```cajeta[[:space:]]*$/ && !inblock {
            skip = (hist2 ~ /<!-- snippet: skip -->/ || prev ~ /<!-- snippet: skip -->/)
            inblock = 1; n += 1
            if (!skip) { file = out "/block" n ".raw"; active = 1 } else active = 0
            next
        }
        /^```[[:space:]]*$/ && inblock { inblock = 0; active = 0; next }
        inblock && active { print > file }
        { prev = hist2 }
    ' "$f"

    local shared="$srcroot/src/snip"
    for raw in "$srcroot"/block*.raw; do
        [ -e "$raw" ] || continue
        n=$((n + 1))
        local pkgdir pkg
        if grep -qE '^[[:space:]]*package[[:space:]]' "$raw"; then
            pkg="$(sed -nE 's/^[[:space:]]*package[[:space:]]+([A-Za-z0-9_.]+);.*/\1/p' "$raw" | head -1)"
            pkgdir="$srcroot/src/$(echo "$pkg" | tr '.' '/')"
            mkdir -p "$pkgdir"
            cp "$raw" "$pkgdir/Block$n.cajeta"
        elif grep -qE '^[[:space:]]*(public[[:space:]]+|final[[:space:]]+|abstract[[:space:]]+)*(class|interface|enum|record|view|structure|annotation)[[:space:]]' "$raw"; then
            mkdir -p "$shared"
            { echo "package snip;"; echo; cat "$raw"; } > "$shared/Block$n.cajeta"
        elif grep -qE '^[[:space:]]*(public|private|protected|static)[^;]*\(' "$raw"; then
            # bare method declaration(s): host class, no method wrapper
            mkdir -p "$shared"
            {
                echo "package snip;"
                echo
                echo "public class Block$n {"
                cat "$raw"
                echo "}"
            } > "$shared/Block$n.cajeta"
        else
            mkdir -p "$shared"
            {
                echo "package snip;"
                echo
                echo "public class Block$n {"
                echo "    public void run() {"
                cat "$raw"
                echo "        return;"
                echo "    }"
                echo "}"
            } > "$shared/Block$n.cajeta"
        fi
        wrote=$((wrote + 1))
    done

    [ "$wrote" -eq 0 ] && return 0

    mkdir -p "$srcroot/src/snipcheck"
    cat > "$srcroot/src/snipcheck/Driver.cajeta" <<'EOF'
package snipcheck;

public final class Driver {
    public static int32 check() {
        return 0;
    }
}
EOF

    checked=$((checked + wrote))
    local log="$srcroot/compile.log"
    # jit-run can exit 0 despite parse/semantic errors in packages the
    # driver never reaches — scan the log for diagnostics too.
    if ! "$CAJETA" jit-run "$srcroot/src" snipcheck.Driver.check >"$log" 2>&1 \
        || grep -qE "line [0-9]+:[0-9]+ |CAJETA_ERROR|extraneous input|no viable alternative|mismatched input" "$log"; then
        echo "SNIPPET FAIL: $f ($wrote block(s) in batch)"
        grep -E "line [0-9]+:[0-9]+ |CAJETA_ERROR|extraneous|no viable|mismatched" "$log" | head -3 | sed 's/^/    /'
        tail -2 "$log" | sed 's/^/    /'
        failures=$((failures + 1))
    fi
}

while IFS= read -r f; do
    [ -z "$f" ] && continue
    grep -q '^```cajeta' "$f" || continue
    check_file "$f"
done < <(collect_files)

if [ "$failures" -gt 0 ]; then
    echo "check-doc-snippets: $failures file(s) with non-compiling blocks ($checked block(s) checked)"
    exit 1
fi
echo "check-doc-snippets: OK ($checked block(s) checked)"
exit 0
