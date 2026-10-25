#!/usr/bin/env bash
# Fetch + checksum-verify the reference datasets (spec §1.4, §3.1, §16).
# Content-addressed: a cached file whose SHA-256 matches the manifest is reused
# (not refetched); a mismatch is treated as stale and refetched; a post-download
# mismatch fails loudly (never reports an unverified dataset). The harness/driver
# point benchmarks at $PROFILE_DATA_DIR.
#
# Usage:  fetch.sh [name]            # all datasets, or just one
# Env:    PROFILE_DATA_DIR (cache dir), PROFILE_MANIFEST (manifest path)
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
MANIFEST="${PROFILE_MANIFEST:-${SCRIPT_DIR}/manifest.tsv}"
CACHE="${PROFILE_DATA_DIR:-${SCRIPT_DIR}/cache}"
ONLY="${1:-}"

fail() { echo "fetch: $1" >&2; exit 1; }
command -v curl      >/dev/null || fail "curl not found"
command -v sha256sum >/dev/null || fail "sha256sum not found"
[[ -f "$MANIFEST" ]] || fail "manifest not found: $MANIFEST"
mkdir -p "$CACHE"

while IFS=$'\t' read -r name url bytes sha; do
    [[ -z "${name:-}" || "$name" == \#* ]] && continue
    [[ -n "$ONLY" && "$ONLY" != "$name" ]] && continue
    dest="$CACHE/$name.json"

    if [[ -f "$dest" ]]; then
        have="$(sha256sum "$dest" | cut -d' ' -f1)"
        if [[ "$have" == "$sha" ]]; then
            echo "[cached]   $name ($bytes bytes)"
            continue
        fi
        echo "[stale]    $name — sha mismatch, refetching"
        rm -f "$dest"
    fi

    echo "[fetch]    $name <- $url"
    curl -fsSL --max-time 180 -o "$dest" "$url" || fail "download failed: $name"
    have="$(sha256sum "$dest" | cut -d' ' -f1)"
    [[ "$have" == "$sha" ]] || fail "CHECKSUM MISMATCH for $name: expected $sha got $have"
    echo "[ok]       $name verified ($bytes bytes)"
done < "$MANIFEST"

echo "fetch: datasets ready in $CACHE"
