// Cajeta build-tool lockfile (`cajeta.lock`) model + I/O.
//
// The lockfile captures resolved state from a `cajeta build` so
// subsequent builds reproduce it exactly:
//   - manifest-checksum   — SHA-256 of the manifest source bytes
//   - resolved-at         — ISO 8601 timestamp of resolution
//   - generator           — { tool, version } of the producer
//   - properties          — resolved property name→value map
//   - packages            — resolved dependency graph (Phase 6)
//   - plugins             — resolved plugin versions (Phase 7)
//   - overrides           — applied transitive-dep overrides (Phase 6)
//
// Strict JSON (no comments — it's machine-only). Lives at the
// project root alongside `cajeta.json`. Committed to VCS.
//
// See BuildTool.md "Lockfile — cajeta.lock" for the spec,
// plan/build-tool-plan.md Phase 2 for context.

#pragma once

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>

namespace cajeta::buildtool {

    // Lockfile data model. Phase 2 populates the top-level metadata
    // + properties; packages/plugins/overrides arrays exist as empty
    // slots ready for Phases 6/7 to fill.
    struct Lockfile {
        int lockfileVersion = 1;
        std::string manifestChecksum;   // "sha256:<hex>" form
        std::string generatorTool = "cajeta";
        std::string generatorVersion;   // CAJETA_VERSION at write time
        std::string resolvedAt;          // ISO 8601, UTC
        std::map<std::string, std::string> properties;

        // Reserved for Phase 6: each entry will be
        // { name, version, resolved-from, checksum, capabilities,
        //   transitive-deps }.
        // Modeled raw for now so the schema slot is on disk and the
        // upgrade path is additive.
        llvm::json::Array packages;

        // Reserved for Phase 7.
        llvm::json::Array plugins;

        // Reserved for Phase 6 — overrides applied to the resolved
        // graph.
        llvm::json::Array overrides;
    };

    // Compute SHA-256 hex digest of a byte string. Returns
    // "sha256:<hex>". The hex is lowercase. Wraps libcrypto's EVP
    // SHA-256 routines (already linked for archive signing).
    std::string sha256Hex(const std::string& bytes);

    // Read a lockfile from disk. Errors when the file is unreadable
    // or doesn't parse as strict JSON of the expected shape.
    llvm::Expected<Lockfile> readLockfile(const std::string& path);

    // Write a lockfile to disk. Format is stable: keys in fixed
    // order, two-space indent, trailing newline. Idempotent —
    // re-writing the same logical lockfile produces byte-identical
    // output.
    llvm::Error writeLockfile(const std::string& path, const Lockfile& lf);

    // Build a Lockfile from a resolved manifest + property set.
    // `manifestSource` is the raw bytes of the manifest file (needed
    // for the checksum). `nowIso` is the timestamp to use (typically
    // current UTC); injecting it lets tests assert determinism.
    Lockfile composeLockfile(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::string& nowIso);

    // Compare a Lockfile's recorded manifest-checksum against the
    // current manifest source. Returns a drift report; if there's no
    // drift the report's `changed` field is false.
    struct DriftReport {
        bool changed = false;
        std::string oldChecksum;
        std::string newChecksum;
    };
    DriftReport checkDrift(const Lockfile& lf, const std::string& currentSource);

    // Current UTC time in ISO 8601 form (e.g. "2026-06-01T00:00:00Z").
    // Helper exposed for tests + the `cajeta info` command.
    std::string nowIsoUtc();

} // namespace cajeta::buildtool
