#!/usr/bin/env bash
# Regenerate the Cajeta API reference (cajetadoc) from the runtime stdlib source
# into site/docs/cajetadocs.
#
# Usage:
#   scripts/regen-cajetadocs.sh [-b|--build]
#
#   -b, --build   Force a rebuild of the cajetadoc tool before generating.
#                 (By default the tool is built only if its binary is missing.)
#
# Overridable via environment:
#   OUT_DIR    output directory      (default: site/docs/cajetadocs)
#   SRC_ROOT   source root to scan   (default: runtime/src)
#   TITLE      header project title  (default: Cajeta)
#   VERSION    header version        (default: contents of ./VERSION)
#   DATE       header publish date   (default: today, YYYY-MM-DD)
#   LICENSE    header license type   (default: Apache-2.0)

set -euo pipefail

# Repo root = parent of this script's directory.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

FORCE_BUILD=0
for arg in "$@"; do
    case "$arg" in
        -b|--build) FORCE_BUILD=1 ;;
        -h|--help)  sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

OUT_DIR="${OUT_DIR:-site/docs/cajetadocs}"
SRC_ROOT="${SRC_ROOT:-runtime/src}"
TITLE="${TITLE:-Cajeta}"
VERSION="${VERSION:-$(tr -d ' \t\n\r' < VERSION 2>/dev/null || echo 0.0.0)}"
DATE="${DATE:-$(date +%F)}"
LICENSE="${LICENSE:-Apache-2.0}"

BIN="build/tools/cajetadoc/cajetadoc"

if [[ ! -d build ]]; then
    echo "error: no build/ directory. Configure the project first (cmake -S . -B build)." >&2
    exit 1
fi

if [[ "$FORCE_BUILD" -eq 1 || ! -x "$BIN" ]]; then
    echo ">> building cajetadoc tool"
    cmake --build build --target cajetadoc -j "$(nproc 2>/dev/null || echo 4)"
fi

if [[ ! -x "$BIN" ]]; then
    echo "error: cajetadoc binary not found at $BIN after build." >&2
    exit 1
fi

echo ">> generating docs"
echo "   source : $SRC_ROOT"
echo "   output : $OUT_DIR"
echo "   header : $TITLE v$VERSION  $DATE  $LICENSE"

"$BIN" "$SRC_ROOT" \
    -o "$OUT_DIR" \
    --project-title "$TITLE" \
    --project-version "$VERSION" \
    --date-published "$DATE" \
    --project-license "$LICENSE"

echo ">> done. Open $OUT_DIR/index.html"
