// Detached ed25519 verification for `.cja` archives: a raw signature over the
// archive bytes, stored beside it as `<archive>.sig`. These answer only "did key
// K sign this?" — WHICH keys are acceptable stays the caller's policy.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Whether raw `signature` verifies over `dataPath` under the PEM at `pemPath`.
    // A false RESULT is an answer; an ERROR means the question could not be asked,
    // and callers must never collapse the two.
    llvm::Expected<bool> verifyDetachedEd25519(
        const std::string& dataPath,
        const std::string& signature,
        const std::string& pemPath);

    // The first key in `pemPaths` that verifies, or nullopt. An unparseable key is
    // SKIPPED, so one bad file in a trust store cannot hide a valid signature.
    llvm::Expected<std::optional<std::string>> verifyAgainstAnyKey(
        const std::string& dataPath,
        const std::string& signature,
        const std::vector<std::string>& pemPaths);

    // As above, over bytes already in hand rather than a file on disk.
    llvm::Expected<bool> verifyDetachedEd25519Bytes(
        const std::string& data,
        const std::string& signature,
        const std::string& pemPath);

    // As above, against a PEM held in memory, such as the embedded trust anchor.
    llvm::Expected<bool> verifyDetachedEd25519PemBytes(
        const std::string& data,
        const std::string& signature,
        const std::string& pemContents);

    // A file's contents against a PEM held in memory, as org key documents supply.
    llvm::Expected<bool> verifyDetachedEd25519File(
        const std::string& dataPath,
        const std::string& signature,
        const std::string& pemContents);

    // Read a detached signature file into raw bytes.
    llvm::Expected<std::string> readSignatureFile(const std::string& path);

} // namespace cajeta::buildtool
