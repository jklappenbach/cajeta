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
#   - statements plus a method        -> a script unit, compiled verbatim
#   - method declaration(s)           -> wrapped in a host class
#   - otherwise (statement fragment)  -> wrapped in a class + method
# A wrapped block's `import` lines hoist to the top of the file.
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

IMPORT_RE='^[[:space:]]*import[[:space:]]'
# A method declaration whose modifiers are omitted: a return type (an
# identifier, optionally `#`-prefixed, with generics or array brackets)
# then a name, a parameter list, and `{` or `;` ending the line.
BARE_METHOD_RE='^[[:space:]]*#?[A-Za-z_][A-Za-z0-9_.]*(<[^;{}()]*>)?([[:space:]]*\[[[:space:]]*\])*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\([^;{}]*\)[[:space:]]*(\{|;)'
STATEMENT_KEYWORD_RE='^[[:space:]]*(if|else|while|for|switch|case|default|catch|try|finally|do|return|throw|new|heap|stack|scope|spawn|assert|import|package|synchronized)\b'

# A statement at column 0: control flow, or a call as its own statement.
TOP_STATEMENT_RE='^(if|while|for|do|switch|try|return|throw)\b|^[A-Za-z_][A-Za-z0-9_.]*[[:space:]]*\('

is_bare_method() { # <raw-block>
    local raw="$1" cand
    grep -qE '^[[:space:]]*(public|private|protected|static)[^;]*\(' "$raw" && return 0
    cand="$(grep -E "$BARE_METHOD_RE" "$raw" | grep -vE "$STATEMENT_KEYWORD_RE")"
    [ -n "$cand" ] && return 0
    return 1
}

is_script_unit() { # <raw-block>: file-scope statements mixed with methods
    local raw="$1"
    is_bare_method "$raw" || return 1
    grep -qE "$TOP_STATEMENT_RE" "$raw" && return 0
    return 1
}

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
        elif is_script_unit "$raw"; then
            # a script unit compiles verbatim, with no package and no host
            # class (docs/specification/lang/ScriptUnits.md)
            mkdir -p "$srcroot/src"
            cp "$raw" "$srcroot/src/Script$n.cajeta"
        elif is_bare_method "$raw"; then
            # bare method declaration(s): host class, no method wrapper
            mkdir -p "$shared"
            {
                echo "package snip;"
                echo
                grep -E "$IMPORT_RE" "$raw"
                echo "public class Block$n {"
                grep -vE "$IMPORT_RE" "$raw"
                echo "}"
            } > "$shared/Block$n.cajeta"
        else
            mkdir -p "$shared"
            {
                echo "package snip;"
                echo
                grep -E "$IMPORT_RE" "$raw"
                echo "public class Block$n {"
                echo "    public void run() {"
                grep -vE "$IMPORT_RE" "$raw"
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
    local rc=0
    "$CAJETA" jit-run "$srcroot/src" snipcheck.Driver.check >"$log" 2>&1 || rc=$?
    # A log that cannot be read is a failure: the diagnostic scan below
    # would find nothing and the file would read green.
    if [ ! -r "$log" ]; then
        echo "SNIPPET FAIL: $f (compile log unreadable: $log)"
        failures=$((failures + 1))
        return 0
    fi
    # jit-run can exit 0 despite parse/semantic errors in packages the
    # driver never reaches — scan the log for diagnostics too.
    if [ "$rc" -ne 0 ] \
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
