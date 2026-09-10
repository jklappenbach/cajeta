// Cajeta build-tool lockfile (`cajeta.lock`) model and I/O: the resolved state
// of a `cajeta build` — checksum, timestamp, generator, properties, packages,
// melts, plugins, overrides. Strict JSON, committed, beside `cajeta.json`.

#pragma once

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>

#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One resolved melt as recorded in the lockfile, mirroring the `melts[]`
    // schema in BuildTool.md.
    struct ResolvedMeltEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;                    // "sha256:<hex>"
        // Concrete `name@version` for this melt's immediate transitives.
        std::vector<std::string> transitiveMelts;
    };

    // One resolved package, with the audit field naming the melt — or
    // "explicit" — that supplied this version.
    struct ResolvedPackageEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;
        std::string providedBy;
        // The owning workspace member; empty, and omitted on disk, otherwise.
        std::string memberOwner;
    };

    // One resolved plugin, mirroring the top-level `plugins` array.
    struct ResolvedPluginEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;
        // Sorted, so a capability addition shows up in every review diff.
        std::vector<std::string> capabilities;
    };

    // The lockfile data model. Typed slots are preferred; the raw ones are the
    // escape hatch for schema revs that have no typed model yet.
    struct Lockfile {
        int lockfileVersion = 1;
        std::string manifestChecksum;   // "sha256:<hex>" form
        std::string generatorTool = "cajeta";
        std::string generatorVersion;   // CAJETA_VERSION at write time
        std::string resolvedAt;          // ISO 8601, UTC
        std::map<std::string, std::string> properties;

        // A non-empty typed vector is written as that array; otherwise the
        // matching raw slot below is written instead.
        std::vector<ResolvedPackageEntry> packagesTyped;
        std::vector<ResolvedMeltEntry> meltsTyped;
        std::vector<ResolvedPluginEntry> pluginsTyped;

        llvm::json::Array packagesRaw;
        llvm::json::Array meltsRaw;
        llvm::json::Array pluginsRaw;

        llvm::json::Array overrides;

        // Workspace-scoped: the document gains a `workspace` block listing the
        // members, and per-member packages stay in the flat `packages` array
        // with a `member` discriminator so tooling works uniformly.
        bool isWorkspace = false;
        struct WorkspaceMemberEntry {
            std::string name;              // memberShortName
            std::string declaredPath;      // workspace-relative
            std::string manifestChecksum;  // "sha256:<hex>"
        };
        std::vector<WorkspaceMemberEntry> workspaceMembers;
    };

    // SHA-256 of `bytes` as "sha256:<lowercase hex>", via libcrypto's EVP.
    std::string sha256Hex(const std::string& bytes);

    // Reads a lockfile; errors when unreadable or not the expected strict JSON.
    llvm::Expected<Lockfile> readLockfile(const std::string& path);

    // Writes a lockfile in a stable format — fixed key order, two-space indent,
    // trailing newline — so re-writing the same content is byte-identical.
    llvm::Error writeLockfile(const std::string& path, const Lockfile& lf);

    // Builds a Lockfile from a resolved manifest and property set: `manifestSource`
    // is the raw manifest bytes, `nowIso` the injectable resolved-at stamp.
    Lockfile composeLockfile(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::string& nowIso);

    struct ResolvedDependency;
    struct MeltResolution;
    struct ResolvedPlugin;
    struct Workspace;

    // Composes a workspace lockfile: the checksum is over the workspace-root
    // manifest and per-member packages flatten into `packagesTyped`.
    // `perMemberDeps` is keyed by member short name; absent members are dropped.
    struct WorkspaceLockfileInputs {
        std::string workspaceManifestSource;
        std::map<std::string, std::vector<ResolvedDependency>>
            perMemberDeps;
        std::map<std::string, std::string> memberManifestSources;
    };
    Lockfile composeWorkspaceLockfile(
        const Workspace* workspace,
        const WorkspaceLockfileInputs& inputs,
        const ResolvedProperties& props,
        const std::string& nowIso);

    // composeLockfile() plus the typed package/melt/plugin slots. `meltProvidedBy`
    // maps a dep to "<melt>@<version>"; a dep absent from it records "explicit".
    Lockfile composeLockfileWithResolution(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::vector<ResolvedDependency>& resolvedDeps,
        const MeltResolution& melts,
        const std::map<std::string, std::string>& meltProvidedBy,
        const std::vector<ResolvedPlugin>& resolvedPlugins,
        const std::string& nowIso);

    // Compares the recorded manifest checksum against the current source;
    // `changed` is false when they agree.
    struct DriftReport {
        bool changed = false;
        std::string oldChecksum;
        std::string newChecksum;
    };
    DriftReport checkDrift(const Lockfile& lf, const std::string& currentSource);

    // Current UTC time in ISO 8601 form, e.g. "2026-06-01T00:00:00Z".
    std::string nowIsoUtc();

} // namespace cajeta::buildtool
