# Trust anchors

`olla-root.pub` is the repository root public key embedded in the
toolchain (publisher-trust spec §3.1). A client verifies an organization
key document against it with no operator action, the way an OS verifies
against a shipped CA bundle.

**Only the PUBLIC key lives here, and that is the point.** The private
half belongs to whoever operates the repository. Nothing built from this
tree can sign as the root, which is why the tests inject their own
"shipped" root rather than using this one — a test that needed the
production private key would only be possible if the private key were
somewhere it must never be.

## Rotation

Replace this file and cut a toolchain release (§3.2). The toolchain's own
distribution is already signed, so the new root arrives over a channel
that is itself authenticated. Clients mid-upgrade keep working because a
document may be signed by any trusted root, and an operator can add the
incoming root ahead of the release with `cajeta trust add-root`.

## Current status

**Production root**, `olla-root-1`, installed 2026-09-02. The private half
is held offline on removable media and never touches a networked machine;
this file is the public half only.

Signing is a manual ceremony — see the operator's `olla-key` toolkit. The
root signs rarely by design: the repository delegation (annually), an
organization key document (per org, plus rotations), and a re-signed
document repairing a revocation. Everything per-publish is signed by the
delegated release key, which lives in olla as a Worker secret.

## Check before you commit

A private key here would build, ship, and verify nothing — the failure is
silent. `-----BEGIN PUBLIC KEY-----` is the only acceptable first line:

    head -1 resources/roots/olla-root.pub
