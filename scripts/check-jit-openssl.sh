#!/usr/bin/env bash
# The cajeta driver's JIT must resolve OpenSSL (libssl and libcrypto) from its
# own process. dev.cajeta.crypto's shim calls both, and under `cajeta jit-run`
# and `cajeta test` the JIT finds them only among the driver's symbols. Until
# the stdlib's TLS left for that library (cajeta-crypto spec 6.3), the
# driver's embedded TLS object held the only libssl reference, so removing it
# could let the linker drop libssl and break every JIT run of a TLS program
# with "Symbols not found: [ TLS_method ... ]". This runs a program that calls
# one function from each library through @Native.
#
#   scripts/check-jit-openssl.sh [path/to/cajeta]
#
# POSIX only: the static Windows cajeta.exe resolves the JIT through its
# symbol bridge, which binds no OpenSSL (cajeta-crypto plan 0.1.2).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
CAJETA="${1:-$here/build/src/cajeta}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src/probe"
cat > "$work/src/probe/Main.cajeta" <<'EOF'
package probe;

public final class Main {
    @Native("TLS_method")
    static pointer tlsMethod() { }

    @Native("OpenSSL_version_num")
    static int64 versionNum() { }

    public static int32 main() {
        pointer m = Main.tlsMethod();
        if (m == null) {
            System.stdout.println("check-jit-openssl: TLS_method answered null");
            return 1;
        }
        System.stdout.println("check-jit-openssl: libssl and libcrypto resolve, OpenSSL " + Main.versionNum());
        return 0;
    }
}
EOF
# On Linux the driver must name libssl itself. Ubuntu's libcurl happens to pull
# it in transitively (measured 2026-10-18), which a GnuTLS curl would not, so a
# passing JIT run alone does not prove the link line keeps it.
if [[ "$(uname -s)" == Linux ]] && command -v readelf >/dev/null 2>&1; then
    # Captured first: under pipefail, `readelf | grep -q` fails when grep exits early.
    dyn="$(readelf -d "$CAJETA")"
    grep -q 'NEEDED.*libssl\.so' <<< "$dyn" \
        || { echo "check-jit-openssl: FAIL, the driver does not link libssl directly (readelf -d shows no NEEDED libssl)" >&2; exit 1; }
fi
out="$("$CAJETA" jit-run "$work/src" probe.Main.main 2>&1)" || true
echo "$out" | tail -3
grep -q "check-jit-openssl: libssl and libcrypto resolve" <<< "$out" \
    || { echo "check-jit-openssl: FAIL, the driver's JIT does not resolve OpenSSL" >&2; exit 1; }
