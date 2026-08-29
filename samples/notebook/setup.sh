#!/usr/bin/env bash
# Stage everything the tour notebook needs, entirely offline.
#
# Builds four small libraries, lays them out as a filesystem repository,
# generates two ed25519 keypairs, and signs with them — one key the machine
# will trust, one it will not. Nothing here touches the network; the tour
# exercises the real resolve/verify/splice path against local bytes.
#
#   ./setup.sh            build everything
#   ./setup.sh --clean    remove what it built
#
# Re-runnable: it rebuilds from scratch each time.

set -uo pipefail
cd "$(dirname "$0")"

REPO=repo
TRUST=trust
KEYS=.keys
WORK=.work

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$REPO" "$TRUST" "$KEYS" "$WORK"
    echo "cleaned."
    exit 0
fi

# The compiler: an explicit CAJETA_BINARY wins, else the repo's build, else
# whatever is on PATH.
if [[ -n "${CAJETA_BINARY:-}" ]]; then
    CAJETA="$CAJETA_BINARY"
elif [[ -x ../../build/src/cajeta ]]; then
    CAJETA=$(cd ../.. && pwd)/build/src/cajeta
else
    CAJETA=$(command -v cajeta || true)
fi
if [[ -z "$CAJETA" || ! -x "$CAJETA" ]]; then
    echo "setup: no cajeta compiler found." >&2
    echo "  build one (./build.sh at the repo root) or set CAJETA_BINARY." >&2
    exit 1
fi
echo ">> compiler: $CAJETA"

command -v openssl >/dev/null || { echo "setup: openssl is required." >&2; exit 1; }

rm -rf "$REPO" "$TRUST" "$KEYS" "$WORK"
mkdir -p "$REPO" "$TRUST" "$KEYS" "$WORK"

# --- keys -------------------------------------------------------------
# `trusted` goes into the trust store the notebook points the kernel at.
# `rogue` never does — it exists so the tour can show a signature that is
# perfectly valid and still refused, which is the case that matters.
openssl genpkey -algorithm ED25519 -out "$KEYS/trusted.pem" 2>/dev/null
openssl pkey -in "$KEYS/trusted.pem" -pubout -out "$TRUST/tour-publisher.pem" 2>/dev/null
openssl genpkey -algorithm ED25519 -out "$KEYS/rogue.pem" 2>/dev/null
echo ">> keys: trust/tour-publisher.pem (trusted), .keys/rogue.pem (not)"

# --- one library ------------------------------------------------------
# $1 package, $2 entry class, $3 signing key ("trusted" | "rogue" | "none"),
# $4.. source files as "RelPath.cajeta:::<source>"
build_lib() {
    local pkg="$1" entry="$2" signing="$3"; shift 3
    local src="$WORK/$pkg/src"
    mkdir -p "$src/$pkg"
    local spec name body
    for spec in "$@"; do
        name="${spec%%:::*}"
        body="${spec#*:::}"
        printf '%s' "$body" > "$src/$pkg/$name"
    done

    local out="$WORK/$pkg/out"
    mkdir -p "$out"
    if ! "$CAJETA" "$pkg.$entry" "$src" "$out" --emit=cja \
            > "$WORK/$pkg/build.log" 2>&1; then
        echo "setup: building $pkg failed — see $WORK/$pkg/build.log" >&2
        exit 1
    fi

    # Filesystem repository layout: <root>/<name>/<version>/<name>-<version>.cja
    local dir="$REPO/$pkg/1.0.0"
    mkdir -p "$dir"
    cp "$out/$pkg.cja" "$dir/$pkg-1.0.0.cja"

    # The checksum the repository publishes. The install verifies against
    # THIS, not against a hash of what it just downloaded.
    openssl dgst -sha256 -r "$dir/$pkg-1.0.0.cja" | cut -d' ' -f1 \
        > "$dir/$pkg-1.0.0.cja.sha256"

    if [[ "$signing" != "none" ]]; then
        "$CAJETA" archive sign "$dir/$pkg-1.0.0.cja" \
            --key "$KEYS/$signing.pem" \
            --out "$dir/$pkg-1.0.0.cja.sig" > /dev/null 2>&1 \
            || { echo "setup: signing $pkg failed" >&2; exit 1; }
    fi
    echo ">> $pkg 1.0.0  (signed: $signing)"
}

build_lib demo Stats trusted \
"Stats.cajeta:::package demo;

public class Stats {
    public static int32 sum(int32 a, int32 b) { return a + b; }
    public static int32 answer() { return 42; }
}
" \
"Greeter.cajeta:::package demo;

public class Greeter {
    public static int32 twice(int32 n) { return n + n; }
}
"

build_lib plain Plain none \
"Plain.cajeta:::package plain;

public class Plain {
    public static int32 value() { return 7; }
}
"

build_lib rogue Sneaky rogue \
"Sneaky.cajeta:::package rogue;

public class Sneaky {
    public static int32 value() { return 13; }
}
"

build_lib coll Marker trusted \
"Marker.cajeta:::package coll;

public class Marker {
    public static int32 value() { return 99; }
}
"

cat <<'DONE'

Staged. Now start the kernel with the trust store pointed here:

    CAJETA_TRUST_KEYS_DIR="$PWD/trust" jupyter lab notebooks/tour.ipynb

(install the kernelspec first if you have not: `cajeta init --kernel`)

Without CAJETA_TRUST_KEYS_DIR the signed installs are refused — which is
correct, and the tour's Part 4 says so.
DONE
