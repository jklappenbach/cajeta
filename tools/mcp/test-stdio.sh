#!/usr/bin/env bash
# Acceptance test for the stdio JSON-RPC transport under REAL client
# semantics: stdin is held open (no EOF) between messages, exactly as an MCP
# client (Claude Code, etc.) drives it. This is the case a one-shot
# `printf ... | cajeta-mcp` pipe does NOT exercise — the pipe hits EOF, which
# masked a blocking-read bug (__cajeta_file_read looping to fill its whole
# buffer would hang a stream peer forever). A regression here shows up as a
# read timeout, not a wrong answer.
#
#   CAJETA — compiler binary (default: ../../build/src/cajeta)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
CAJETA="${CAJETA:-$here/../../build/src/cajeta}"
MCP="$here/build/cajeta-mcp"

[[ -x "$CAJETA" ]] || { echo "no compiler at $CAJETA (set CAJETA)"; exit 1; }
if [[ ! -x "$MCP" ]]; then
    echo ">> building cajeta-mcp"
    ( cd "$here" && "$CAJETA" build >/dev/null )
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

in=$(mktemp -u); out=$(mktemp -u); mkfifo "$in" "$out"
"$MCP" --cajeta="$CAJETA" --no-cache <"$in" >"$out" 2>/dev/null &
srv=$!
trap 'kill $srv 2>/dev/null || true; rm -f "$in" "$out"' EXIT
exec 3>"$in"     # hold stdin open — the server must never see EOF here
exec 4<"$out"

send() { printf '%s\n' "$1" >&3; }
# Read one response line with a hard timeout; a hang (the bug) fails fast.
recv() { local line; if IFS= read -r -t 8 line <&4; then printf '%s' "$line"; else return 1; fi; }

# 1) initialize must answer while stdin stays open.
send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}'
resp="$(recv)" || fail "no initialize response within 8s (stdin held open) — blocking-read regression"
grep -q '"id":1' <<<"$resp"      || fail "initialize: wrong id: $resp"
grep -q 'serverInfo' <<<"$resp"  || fail "initialize: missing serverInfo: $resp"
echo "ok: initialize answered with stdin held open"

# 2) lifecycle continues in the same session: notification (silent) + tools/list.
send '{"jsonrpc":"2.0","method":"notifications/initialized"}'
send '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
resp="$(recv)" || fail "no tools/list response within 8s — notification mis-framed or blocking-read regression"
grep -q '"id":2' <<<"$resp"        || fail "tools/list: wrong id (notification got a reply?): $resp"
grep -q '"jit_execute"' <<<"$resp" || fail "tools/list: missing tools: $resp"
echo "ok: notification silent + tools/list answered in-session"

echo "ALL STDIO TRANSPORT TESTS PASSED"
