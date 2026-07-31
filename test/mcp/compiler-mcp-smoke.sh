#!/usr/bin/env bash
# compiler-mcp Unit 3 acceptance (spec 8.3): drive `cajeta compiler-mcp` over
# stdio under real client semantics — stdin held open between messages (the
# tools/mcp/test-stdio.sh pattern). One handshake + one call per tool.
#
#   CAJETA — compiler binary (default: ../../build/src/cajeta)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
CAJETA="${CAJETA:-$here/../../build/src/cajeta}"
[[ -x "$CAJETA" ]] || { echo "no compiler at $CAJETA (set CAJETA)"; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }

work="$(mktemp -d)"   # no project, no lockfile — embedded corpus only
in=$(mktemp -u); out=$(mktemp -u); mkfifo "$in" "$out"
( cd "$work" && "$CAJETA" compiler-mcp <"$in" >"$out" 2>/dev/null ) &
srv=$!
trap 'kill $srv 2>/dev/null || true; rm -f "$in" "$out"; rm -rf "$work"' EXIT
exec 3>"$in"     # hold stdin open — the server must never see EOF here
exec 4<"$out"

send() { printf '%s\n' "$1" >&3; }
recv() { local line; if IFS= read -r -t 8 line <&4; then printf '%s' "$line"; else return 1; fi; }

# 1) initialize answers while stdin stays open.
send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}'
resp="$(recv)" || fail "no initialize response within 8s (stdin held open)"
grep -q '"id":1' <<<"$resp"              || fail "initialize: wrong id: $resp"
grep -q '"compiler-mcp"' <<<"$resp"      || fail "initialize: wrong identity: $resp"
grep -q 'searchSkills' <<<"$resp"        || fail "initialize: missing instructions: $resp"

# 2) tools/list names exactly the three skill tools.
send '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
resp="$(recv)" || fail "no tools/list response"
for t in searchSkills listSkills getSkills; do
    grep -q "\"$t\"" <<<"$resp" || fail "tools/list: missing $t: $resp"
done

# 3) searchSkills (typo'd), listSkills (scoped), getSkills (batch).
send '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"searchSkills","arguments":{"name":"cajeta/toolchan/jit-run"}}}'
resp="$(recv)" || fail "no searchSkills response"
grep -q 'cja-skill://cajeta.toolchain@' <<<"$resp" || fail "searchSkills: no toolchain URI: $resp"

send '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"listSkills","arguments":{"scope":"cajeta/toolchain"}}}'
resp="$(recv)" || fail "no listSkills response"
grep -q 'cajeta-driver-overview' <<<"$resp" || fail "listSkills: overview missing: $resp"

send '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"getSkills","arguments":{"uris":["cja-skill://cajeta.toolchain@1.0/cajeta-driver-overview"]}}}'
resp="$(recv)" || fail "no getSkills response"
grep -q '"ok":true' <<<"$resp" || fail "getSkills: payload not ok: $resp"

# 4) EOF → clean exit.
exec 3>&-
wait "$srv" || fail "server exited non-zero on EOF"
echo "PASS: compiler-mcp stdio smoke"
