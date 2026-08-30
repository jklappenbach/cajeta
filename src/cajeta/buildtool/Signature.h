// Detached ed25519 signature verification for `.cja` archives.
//
// The format is the one `cajeta archive sign` already produces and
// `cajeta archive verify-sig` already checks: a raw ed25519 signature over
// the archive's bytes, stored beside it as `<archive>.sig`. This header
// exists because that logic lived inside the CLI command, and verification
// belongs on every path that ACQUIRES an archive — the build tool's fetch
// and a notebook's `Packages.install` alike — not only on the path where a
// human types `verify-sig`.
//
// Trust is deliberately not decided here. These functions answer "did key
// K sign this?"; WHICH keys are acceptable is the caller's policy, and for
// every current caller that means the trust store (`cajeta trust`).

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // True when `signature` (raw ed25519 bytes) is a valid signature over
    // the contents of `dataPath` under the public key PEM at `pemPath`.
    //
    // A false RESULT means the signature did not verify — that is an
    // answer, not a failure. An ERROR means the question could not be
    // asked at all: unreadable file, malformed PEM, or a key that is not
    // ed25519. Callers must not collapse the two: "we could not check"
    // is not "it is fine".
    llvm::Expected<bool> verifyDetachedEd25519(
        const std::string& dataPath,
        const std::string& signature,
        const std::string& pemPath);

    // Try every key in `pemPaths` and return the path of the first that
    // verifies, or nullopt when none does. Keys that cannot be parsed are
    // SKIPPED rather than fatal: one unreadable file in a trust store must
    // not make an otherwise-valid signature unverifiable.
    llvm::Expected<std::optional<std::string>> verifyAgainstAnyKey(
        const std::string& dataPath,
        const std::string& signature,
        const std::vector<std::string>& pemPaths);

    // As above, over bytes already in hand rather than a file. The
    // organization key document signs a payload it carries inline, so
    // there is nothing on disk to point at.
    llvm::Expected<bool> verifyDetachedEd25519Bytes(
        const std::string& data,
        const std::string& signature,
        const std::string& pemPath);

    // As above, against a PEM held in memory. The shipped trust anchor is
    // embedded in the binary, so there is no file to name.
    llvm::Expected<bool> verifyDetachedEd25519PemBytes(
        const std::string& data,
        const std::string& signature,
        const std::string& pemContents);

    // A file's contents against a PEM held in memory — the shape an
    // organization key document produces, since its keys arrive over the
    // wire and never touch disk.
    llvm::Expected<bool> verifyDetachedEd25519File(
        const std::string& dataPath,
        const std::string& signature,
        const std::string& pemContents);

    // Read a detached signature file into raw bytes.
    llvm::Expected<std::string> readSignatureFile(const std::string& path);

} // namespace cajeta::buildtool
