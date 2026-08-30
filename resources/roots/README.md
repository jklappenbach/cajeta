# Trust anchors

`olla-root.pem` is the repository root public key embedded in the
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

**This is a DEVELOPMENT root.** It is a placeholder until
olla.cajeta.dev has a production root, and no real artifact is signed by
it. Replacing it is a file swap plus a release, which is exactly the
rotation path above — the mechanism is the deliverable, not this key.
