// Cajeta build-tool workspace model — Phase 12.
//
// A workspace coordinates a monorepo of packages. The workspace
// manifest lives at the workspace root and carries a `workspace`
// block whose two fields are:
//
//   - `members`              — array of relative-path member
//                              directories (each containing its
//                              own `cajeta.json` member manifest).
//   - `shared-dependencies`  — version-constraint table merged into
//                              every member's dependency view.
//
// See cajeta-docs/BuildTool.md "Workspaces" for the spec,
// plan/build-tool-plan.md Phase 12 for context.
//
// The workspace surfaces three uses to callers:
//
//   1. `cajeta workspace build / publish [-p member]` — top-level
//      subcommands that build / publish every member (or one) from
//      anywhere inside the workspace.
//   2. `cajeta run-task <member>:<task>` — cross-member task
//      invocation. The colon form resolves to the named member's
//      manifest before dispatching.
//   3. Member-task shadow — a workspace task with the same name as
//      a member task yields to the member task for that member's
//      invocations.
//
// Mutually exclusive with `melt` (rejected at manifest load).

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One curated `name -> version-constraint` from
    // `workspace.shared-dependencies`. Declaration order is
    // preserved so error reporting can cite the original location.
    struct WorkspaceSharedDependency {
        std::string name;
        std::string versionConstraint;
    };

    // One loaded workspace member. The member manifest is parsed
    // eagerly so structural errors surface at workspace-load time
    // rather than at first task invocation.
    struct WorkspaceMember {
        // Path as written in `workspace.members`. Kept verbatim so
        // error messages cite what the user typed.
        std::string declaredPath;
        // Member directory, absolute.
        std::string absPath;
        // `<absPath>/cajeta.json`.
        std::string manifestPath;
        // Parsed member manifest. Carries `details.name` /
        // `details.version` used as the lookup key for the
        // `<member>:<task>` form (matches on `details.name`'s last
        // segment, or on `declaredPath`'s leaf directory name).
        Manifest manifest;
    };

    struct Workspace {
        // Absolute path to the directory containing the workspace
        // manifest. Equals `${workspace.root}` everywhere.
        std::string rootPath;
        // Absolute path of the workspace root's `cajeta.json`.
        std::string manifestPath;
        // The root manifest (the one whose `workspace` block was
        // parsed). Kept so callers don't have to re-load it.
        Manifest rootManifest;
        // Patterns from `workspace.members`, in declaration order.
        std::vector<std::string> memberPatterns;
        // Loaded members, in declaration order. Reverse-lookup by
        // `memberShortName` below.
        std::vector<WorkspaceMember> members;
        // Curated cross-workspace constraints.
        std::vector<WorkspaceSharedDependency> sharedDependencies;
    };

    // Short name a member is addressable by from the workspace CLI
    // (`-p <name>` and `<name>:<task>`). Defaults to the member
    // directory's leaf segment; falls back to the manifest's
    // `details.library()` if the dir leaf wasn't a useful name.
    // Stable across runs as long as the source layout is stable.
    std::string memberShortName(const WorkspaceMember& m);

    // Parse the `workspace` block out of a root manifest into the
    // typed model. The returned Workspace has its `rootManifest` /
    // `rootPath` populated and its `memberPatterns` / `sharedDeps`
    // captured — but `members` is left empty. Use loadWorkspace()
    // to also load each member's manifest.
    //
    // Errors:
    //   - manifest doesn't have a `workspace` block
    //   - `workspace.members` missing / not an array / non-string entry
    //   - `workspace.shared-dependencies` not an object
    //   - unknown subfield under `workspace.*`
    llvm::Expected<Workspace> parseWorkspace(const Manifest& root);

    // Load a workspace by parsing the root manifest and loading
    // each member manifest. `rootManifestPath` must point at the
    // workspace-root `cajeta.json` (the one carrying the
    // `workspace` block). The returned Workspace has all members
    // loaded and validated.
    llvm::Expected<Workspace> loadWorkspace(
        const std::string& rootManifestPath);

    // Walk upward from `startDir` looking for the nearest ancestor
    // (inclusive) whose `cajeta.json` carries a `workspace` block.
    // Returns the absolute manifest path, or std::nullopt when no
    // workspace root exists on the ancestry chain.
    //
    // Used by `cajeta workspace …` to let users invoke the command
    // from anywhere inside the workspace.
    std::optional<std::string> discoverWorkspaceRoot(
        const std::string& startDir);

    // Phase 12 incremental-rebuild predicate.
    //
    // Returns the set of member short names that need a rebuild
    // given a previously-recorded workspace lockfile. A member
    // needs a rebuild when:
    //
    //   - the lockfile has no record of it (new member), OR
    //   - the recorded manifest-checksum doesn't match the current
    //     manifest source, OR
    //   - any intra-workspace dependency of the member needs a
    //     rebuild (transitive — downstream propagation).
    //
    // Members whose manifest checksums match AND whose intra-
    // workspace deps are all clean are returned absent from the
    // result, so callers can short-circuit `build` / `publish`.
    //
    // `lockfile` is an optional argument — pass nullptr to force
    // every member to rebuild (the "no prior lockfile" case).
    struct LockfileWorkspaceView {
        std::map<std::string, std::string> recordedChecksums;
    };
    LockfileWorkspaceView extractWorkspaceLockView(
        const struct Lockfile& lf);
    std::set<std::string> membersNeedingRebuild(
        const Workspace& ws,
        const LockfileWorkspaceView* lockfileView);

    // Topologically sort members by intra-workspace dependency
    // edges. Member A depends on member B when A's
    // `settings.dependencies` declares a name matching one of the
    // workspace's members (matched by `details.name`'s `library()`
    // segment or by `memberShortName`).
    //
    // Returned order is build order: depended-upon members come
    // before their consumers. Pure cycles produce an error citing
    // both ends.
    llvm::Expected<std::vector<const WorkspaceMember*>>
    topologicallySortMembers(const Workspace& ws);

} // namespace cajeta::buildtool
