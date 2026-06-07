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
// plans/buildtool/build-tool-plan.md Phase 2 for context.

#pragma once

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One resolved melt as recorded in the lockfile. Mirrors the
    // `melts[]` schema in BuildTool.md "Lockfile".
    struct ResolvedMeltEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;                    // "sha256:<hex>"
        // Concrete `name@version` strings for the immediate transitives
        // declared by this melt's `melt.melts`.
        std::vector<std::string> transitiveMelts;
    };

    // One resolved package in the lockfile, including the audit field
    // that names which melt (or "explicit") supplied this version.
    struct ResolvedPackageEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;
        // "explicit" when the consumer pinned it directly; otherwise
        // "<melt-name>@<melt-version>" of the supplying melt.
        std::string providedBy;
        // Phase 12: when this lockfile is workspace-scoped, names
        // the member that owns this entry. Empty for non-workspace
        // (single-package) lockfiles. The disk shape omits the
        // field when empty so single-package lockfiles stay
        // byte-identical to their pre-Phase-12 form.
        std::string memberOwner;
    };

    // One resolved plugin in the lockfile. Mirrors the top-level
    // `plugins` array per BuildTool.md "Plugin declaration".
    struct ResolvedPluginEntry {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string checksum;
        // Sorted list of the plugin's declared capabilities. Recorded
        // so reviewers see capability additions on every PR — same
        // motivation as recording resolved versions.
        std::vector<std::string> capabilities;
    };

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

        // Phase 6 typed entries. When `packagesTyped` is non-empty,
        // the writer emits them as the `packages` array; otherwise it
        // falls back to `packagesRaw` (the historical raw slot). Same
        // pattern for `meltsTyped` / `meltsRaw` and `pluginsTyped` /
        // `pluginsRaw`.
        std::vector<ResolvedPackageEntry> packagesTyped;
        std::vector<ResolvedMeltEntry> meltsTyped;
        // Phase 7 typed entries.
        std::vector<ResolvedPluginEntry> pluginsTyped;

        // Raw escape hatches for the slots that don't yet have typed
        // models. Kept so the lockfile schema rev can extend
        // additively without touching every caller.
        llvm::json::Array packagesRaw;
        llvm::json::Array meltsRaw;
        llvm::json::Array pluginsRaw;

        // Reserved for Phase 6 — overrides applied to the resolved
        // graph.
        llvm::json::Array overrides;

        // Phase 12: workspace-scoped lockfile. When `isWorkspace` is
        // true the document carries a top-level `workspace` block
        // whose `members` array lists each member by short name +
        // manifest checksum. Per-member resolved packages live in
        // the same flat `packages` array with a `member` field
        // discriminator so the surrounding tooling (drift checks,
        // provided-by audit) operates uniformly across workspaces +
        // single-package projects.
        bool isWorkspace = false;
        struct WorkspaceMemberEntry {
            std::string name;              // memberShortName
            std::string declaredPath;      // workspace-relative
            std::string manifestChecksum;  // "sha256:<hex>"
        };
        std::vector<WorkspaceMemberEntry> workspaceMembers;
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

    // Forward declarations for the resolver/melt/plugin types are
    // pulled in via the corresponding TU; using forward decls keeps
    // this header lean for unit-test files that don't need them.
    struct ResolvedDependency;
    struct MeltResolution;
    struct ResolvedPlugin;
    struct Workspace;

    // Phase 12: compose a workspace lockfile from per-member
    // resolutions. The result's `manifestChecksum` is computed over
    // the workspace-root manifest source. Per-member packages are
    // flattened into `packagesTyped` with a `memberOwner` annotation
    // (carried via ResolvedPackageEntry.providedBy when no melt
    // supplied the dep — for workspace mode the entry's `member`
    // field on disk distinguishes ownership).
    //
    // `members` is the loaded workspace member list. `perMemberDeps`
    // is indexed by `memberShortName(member)` — entries for absent
    // members are silently dropped (member may legitimately have no
    // deps). `nowIso` is the resolved-at timestamp.
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

    // Same as composeLockfile() but populates the typed packages +
    // melts + plugins slots from the resolver outputs.
    // `meltProvidedBy` maps dep name → "<melt-name>@<melt-version>";
    // deps not in that map get "explicit" as their provided-by.
    Lockfile composeLockfileWithResolution(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::vector<ResolvedDependency>& resolvedDeps,
        const MeltResolution& melts,
        const std::map<std::string, std::string>& meltProvidedBy,
        const std::vector<ResolvedPlugin>& resolvedPlugins,
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
