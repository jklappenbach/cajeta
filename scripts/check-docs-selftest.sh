#!/usr/bin/env bash
# Self-test for the docs checkers (docs-refactor plan 2.1.1, 2.1.2).
# Builds fixture trees, asserts each checker passes on the good tree
# and fails on the bad one. Exit 0 = all assertions hold.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
assert() { # <desc> <expected-exit> <actual-exit>
    if [ "$2" != "$3" ]; then
        echo "SELFTEST FAIL: $1 (expected exit $2, got $3)"
        fails=$((fails + 1))
    fi
}

# --- link checker fixtures (2.1.1) ---------------------------------
mkdir -p "$TMP/good" "$TMP/bad"
echo "world" > "$TMP/good/target.md"
echo "[ok](target.md)" > "$TMP/good/index.md"
echo "[broken](missing.md)" > "$TMP/bad/index.md"

"$SCRIPT_DIR/check-doc-links.sh" "$TMP/good" >/dev/null 2>&1
assert "link checker passes valid tree" 0 $?
"$SCRIPT_DIR/check-doc-links.sh" "$TMP/bad" >/dev/null 2>&1
assert "link checker fails broken link" 1 $?

# --- snippet checker fixtures (2.1.2) ------------------------------
mkdir -p "$TMP/snip-good" "$TMP/snip-bad"
cat > "$TMP/snip-good/doc.md" <<'EOF'
A compiling class snippet:

```cajeta
public class Ok {
    public int32 n;
    public Ok() { return; }
}
```

A bare method declaration (host-class wrap, later block sees Ok):

```cajeta
public int32 useOk() {
    Ok o = heap Ok();
    return o.n;
}
```

A skipped fragment:

<!-- snippet: skip -->
```cajeta
this is intentionally not compilable
```
EOF
cat > "$TMP/snip-bad/doc.md" <<'EOF'
```cajeta
public class Bad { definitely not cajeta !!! }
```
EOF

"$SCRIPT_DIR/check-doc-snippets.sh" "$TMP/snip-good" >/dev/null 2>&1
assert "snippet checker passes compiling block + skip marker" 0 $?
"$SCRIPT_DIR/check-doc-snippets.sh" "$TMP/snip-bad" >/dev/null 2>&1
assert "snippet checker fails non-compiling block" 1 $?

# --- docstring example lint fixtures (2.2.3) ------------------------
mkdir -p "$TMP/lint-good" "$TMP/lint-bad"
cat > "$TMP/lint-good/A.cajeta" <<'EOF'
/**
 * int8[] digest = Blake3.hash(bytes, len);
 * (HttpRequest) -> #HttpResponse h = srv.handler;   // valid: lambda return type
 */
public class A {}
EOF
cat > "$TMP/lint-bad/B.cajeta" <<'EOF'
/**
 * #int8[] digest = Blake3.hash(bytes, len);
 */
public class B {}
EOF

"$SCRIPT_DIR/check-docstring-examples.sh" "$TMP/lint-good" >/dev/null 2>&1
assert "docstring lint passes clean + lambda-return case" 0 $?
"$SCRIPT_DIR/check-docstring-examples.sh" "$TMP/lint-bad" >/dev/null 2>&1
assert "docstring lint fails #-on-local example" 1 $?

if [ "$fails" -eq 0 ]; then
    echo "docs checker self-test: all assertions passed"
    exit 0
fi
echo "docs checker self-test: $fails assertion(s) failed"
exit 1
