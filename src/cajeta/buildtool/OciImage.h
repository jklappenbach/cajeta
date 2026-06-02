// Minimal OCI image-layout writer.
//
// The `package` action's `format: "container"` target produces an
// OCI v1 image-layout directory:
//
//   oci-layout                          { "imageLayoutVersion": "1.0.0" }
//   index.json                          { "manifests": [ <manifest descriptor> ] }
//   blobs/sha256/<digest>               manifest JSON / config JSON / layer tar.gz
//
// `docker run` / `podman run` consume image-layout directories via
// `docker run --rm $(podman load <image-layout>)`. The acceptance
// path is: build → package container → upload to a mock registry.
// This writer is the bare-minimum producer; richer image features
// (multi-arch, signatures, attestations) are deferred follow-ons.
//
// See https://github.com/opencontainers/image-spec/blob/main/image-layout.md
// for the full spec.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct OciImageSpec {
        // Path of the executable on the host that should land at
        // `/usr/local/bin/<name>` inside the layer. Must exist.
        std::string executablePath;
        // The basename used inside the container (also the ENTRYPOINT
        // command). When empty, derived from executablePath's filename.
        std::string entrypointName;
        // Base image hint stored in the config's `History` block —
        // documents the chosen base for traceability. Today we always
        // produce a single-layer scratch-style image; the field is a
        // record, not a build directive.
        std::string baseHint = "scratch";
        // OS / architecture tags written into the config. Default to
        // host triple values.
        std::string os = "linux";
        std::string arch = "amd64";
        // Container-level metadata. `expose` declares listener ports
        // ("8080/tcp"). `env` and `labels` are passed through to the
        // config block.
        std::vector<std::string> expose;
        std::map<std::string, std::string> env;
        std::map<std::string, std::string> labels;
        // Tag the manifest descriptor carries
        // (org.opencontainers.image.ref.name). Default: "latest".
        std::string tag = "latest";
    };

    struct OciImageResult {
        // Path to the image-layout directory just written.
        std::string layoutDir;
        // Manifest descriptor's digest (sha256:<hex>). What a registry
        // tag-points-at.
        std::string manifestDigest;
        // Config blob's digest. Used by tools that inspect runtime
        // metadata without pulling the full image.
        std::string configDigest;
        // Layer blob digest (the rootfs tar.gz).
        std::string layerDigest;
        // Size in bytes of each blob — what registry uploads need to
        // declare ahead of body bytes.
        long long manifestSize = 0;
        long long configSize = 0;
        long long layerSize = 0;
    };

    // Write a minimal OCI image-layout to `outDir`. Creates the
    // directory if missing; reuses existing blobs when their content
    // is already there (content-addressed). Returns digests + sizes.
    llvm::Expected<OciImageResult> writeOciImage(
        const std::string& outDir,
        const OciImageSpec& spec);

} // namespace cajeta::buildtool
