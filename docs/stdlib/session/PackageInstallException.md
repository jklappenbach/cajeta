# PackageInstallException

`cajeta.session.PackageInstallException` — a
[Packages](Packages.md)`.install` that could not proceed. Subtype of
[RecoverableException](../error/RecoverableException.md), because a failed
install leaves the session serving: catching one and carrying on is the
expected thing to do.

```cajeta
import cajeta.session.Packages;

try {
    Packages.install("dev.cajeta.ml", "0.10.*");
} catch (PackageInstallException e) {
    String why = e.message;   // the session is still usable
}
```

## What throws it

| Cause | |
|---|---|
| No live session | `install` was called somewhere with no session to install into — `cajeta run`, a compiled binary. Declare the dependency in `cajeta.json` instead |
| Nothing satisfies the constraint | The message names the constraint and every repository consulted |
| Version conflict | The archive is already loaded at a version this constraint excludes. A session cannot replace a loaded archive; the message says a restart is required |
| Checksum mismatch | The fetched bytes do not match the repository's published sha256. They are discarded and nothing is installed |
| Signature rejected | The archive is signed, but by no key in the machine's trust store — or it is signed and the machine trusts no keys at all, so the signature cannot be checked |
| `require-signatures` | The governing manifest requires a signature and the archive publishes none |
| Name collision | The archive declares a class the session already holds. An install never shadows session state |
| No governing project | `installAndSave` with no `cajeta.json` to write to |

## Methods

| Signature | |
|---|---|
| `PackageInstallException(#String message)` ⚑ | Wrap a heap `#String` message naming what was rejected and why |
