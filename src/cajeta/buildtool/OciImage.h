// Minimal OCI v1 image-layout writer for the `package` action's `format: "container"`:
// oci-layout + index.json + content-addressed blobs/sha256/<digest>. Single-layer only;
// multi-arch, signatures and attestations are deferred. See the OCI image-layout spec.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct OciImageSpec {
        // Host path of the executable, landing at `/usr/local/bin/<name>`. Must exist.
        std::string executablePath;
        // Basename inside the container and the ENTRYPOINT; defaults to the filename.
        std::string entrypointName;
        // Base image recorded in the config's History block - a record, not a directive.
        std::string baseHint = "scratch";
        // OS / architecture tags written into the config; default to the host triple.
        std::string os = "linux";
        std::string arch = "amd64";
        // Container metadata: `expose` declares listener ports ("8080/tcp").
        std::vector<std::string> expose;
        std::map<std::string, std::string> env;
        std::map<std::string, std::string> labels;
        // Tag on the manifest descriptor (org.opencontainers.image.ref.name).
        std::string tag = "latest";
    };

    struct OciImageResult {
        std::string layoutDir;
        // Manifest descriptor digest (sha256:<hex>) - what a registry tag points at.
        std::string manifestDigest;
        // Config blob digest, for tools inspecting metadata without pulling the image.
        std::string configDigest;
        std::string layerDigest;
        // Blob sizes in bytes, which registry uploads declare ahead of the body.
        long long manifestSize = 0;
        long long configSize = 0;
        long long layerSize = 0;
    };

    // Write a minimal OCI image-layout to `outDir`, creating it if missing and reusing
    // blobs already present (content-addressed). Returns the digests and sizes.
    llvm::Expected<OciImageResult> writeOciImage(
        const std::string& outDir,
        const OciImageSpec& spec);

} // namespace cajeta::buildtool
