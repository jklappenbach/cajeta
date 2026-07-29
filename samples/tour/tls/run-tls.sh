#!/usr/bin/env bash
# Build + run the Cajeta TLS tour.
#
# Loopback-only (no external network). Needs a self-signed certificate; this
# script generates an ephemeral one with openssl (CN/SAN localhost) into
# build/certs/ before running. If openssl is unavailable the demo still
# builds, and the binary skips gracefully.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "${SCRIPT_DIR}/../../.." &> /dev/null && pwd )"
CAJETA="${CAJETA:-${REPO_ROOT}/build/src/cajeta}"

if [[ ! -x "$CAJETA" ]]; then
    echo "error: cajeta compiler not found at $CAJETA" >&2
    echo "       build the compiler first: cd $REPO_ROOT && ./build.sh" >&2
    echo "       or override with CAJETA=/path/to/cajeta" >&2
    exit 1
fi

cd "$SCRIPT_DIR"
"$CAJETA" build

CERT_DIR="build/certs"
if command -v openssl >/dev/null 2>&1; then
    mkdir -p "$CERT_DIR"
    if [[ ! -f "$CERT_DIR/cert.pem" || ! -f "$CERT_DIR/key.pem" ]]; then
        echo "[certs] generating ephemeral self-signed cert (CN/SAN localhost)"
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
            -keyout "$CERT_DIR/key.pem" -out "$CERT_DIR/cert.pem" \
            -days 1 -nodes -subj "/CN=localhost" \
            -addext "subjectAltName=DNS:localhost" \
            >/dev/null 2>&1
    fi
else
    echo "[certs] openssl not found — the demo will skip the live handshake"
fi

exec ./build/tls-tour
