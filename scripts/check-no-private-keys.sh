#!/usr/bin/env sh
# Refuse any private key material under resources/.
#
# resources/roots/ holds PUBLIC trust anchors. A private key there builds,
# ships, and verifies nothing — the failure is silent, which is why this is
# a gate rather than a note in a README. It nearly happened on 2026-09-02:
# the source and destination filenames matched, so `cp` did the wrong thing
# and nothing complained.
#
# Run standalone, or install as a pre-commit hook:
#   ln -sf ../../scripts/check-no-private-keys.sh .git/hooks/pre-commit

set -eu
root=$(git rev-parse --show-toplevel)
bad=0

# Tracked files AND anything staged — a newly added private key is not yet
# tracked, and that is precisely the commit worth stopping.
files=$( { git -C "$root" ls-files resources/
           git -C "$root" diff --cached --name-only -- resources/ ; } | sort -u)

for f in $files; do
    [ -f "$root/$f" ] || continue
    case $(head -1 "$root/$f" 2>/dev/null || true) in
        *"PRIVATE KEY"*)
            echo "PRIVATE KEY in $f — resources/ is public material only" >&2
            bad=1
            ;;
    esac
done

# The anchor itself must be a public key, not merely not-private: an empty
# or truncated file would pass the check above and embed nothing.
anchor="$root/resources/roots/olla-root.pub"
if [ -f "$anchor" ]; then
    case $(head -1 "$anchor") in
        "-----BEGIN PUBLIC KEY-----") ;;
        *) echo "$anchor is not a PEM public key" >&2; bad=1 ;;
    esac
fi

exit $bad
