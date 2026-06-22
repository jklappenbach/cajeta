#!/usr/bin/env bash
# Acceptance test for the MCP server's Streamable-HTTP transport (--http=PORT,
# mcp-compression unit 2). Starts the server, drives POST /mcp with curl, and
# asserts JSON-RPC parity with stdio + the right HTTP status codes (lifecycle,
# method/path errors, notification). The C++ GTest harness (test/mcp) covers the
# stdio path; this covers HTTP, which needs a live socket + background server.
#
#   CAJETA  — compiler binary (default: ../../build/src/cajeta)
#   PORT    — bind port (default: 38251)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
CAJETA="${CAJETA:-$here/../../build/src/cajeta}"
MCP="$here/build/cajeta-mcp"
PORT="${PORT:-38251}"
URL="http://127.0.0.1:$PORT/mcp"

[[ -x "$CAJETA" ]] || { echo "no compiler at $CAJETA (set CAJETA)"; exit 1; }
if [[ ! -x "$MCP" ]]; then
    echo ">> building cajeta-mcp"
    ( cd "$here" && "$CAJETA" build >/dev/null )
fi

fail() { echo "FAIL: $*" >&2; exit 1; }
post() { curl -s -X POST "$URL" --data "$1"; }
status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

"$MCP" --cajeta="$CAJETA" --no-cache --http="$PORT" & SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
    curl -s -o /dev/null -X POST "$URL" --data '{}' && break || sleep 0.1
done

# 1) initialize: HTTP body byte-identical to the stdio response line.
req='{"jsonrpc":"2.0","id":1,"method":"initialize"}'
http_body="$(post "$req")"
stdio_body="$(printf '%s\n' "$req" | "$MCP" --cajeta="$CAJETA" --no-cache 2>/dev/null | tr -d '\n')"
[[ "$http_body" == "$stdio_body" ]] || fail "initialize body != stdio"
echo "ok: initialize parity with stdio"

# 2) tools/list returns the five tools.
post '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | grep -q '"jit_execute"' \
    || fail "tools/list missing tools"
echo "ok: tools/list"

# 3) unknown method -> JSON-RPC -32601 in a 200 body.
post '{"jsonrpc":"2.0","id":3,"method":"bogus"}' | grep -q '"code":-32601' \
    || fail "bogus method should be -32601"
[[ "$(status -X POST "$URL" --data '{"jsonrpc":"2.0","id":3,"method":"bogus"}')" == 200 ]] \
    || fail "bogus method should be HTTP 200"
echo "ok: unknown method -> -32601 / 200"

# 4) wrong HTTP method -> 405.
[[ "$(status -X GET "$URL")" == 405 ]] || fail "GET should be 405"
echo "ok: GET -> 405"

# 5) wrong path -> 404.
[[ "$(status -X POST "http://127.0.0.1:$PORT/nope" --data '{}')" == 404 ]] \
    || fail "wrong path should be 404"
echo "ok: wrong path -> 404"

# 6) notification (no id) -> 202 with an empty body.
[[ -z "$(post '{"jsonrpc":"2.0","method":"notifications/initialized"}')" ]] \
    || fail "notification should have an empty body"
[[ "$(status -X POST "$URL" --data '{"jsonrpc":"2.0","method":"notifications/initialized"}')" == 202 ]] \
    || fail "notification should be HTTP 202"
echo "ok: notification -> 202 empty"

echo "ALL HTTP TRANSPORT TESTS PASSED"
