// Cajeta build-tool workspace model: a root manifest whose `workspace` block
// names its member directories and the shared dependency constraints merged
// into each. See docs/BuildTool.md "Workspaces"; exclusive with `melt`.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One `workspace.shared-dependencies` entry, kept in declaration order.
    struct WorkspaceSharedDependency {
        std::string name;
        std::string versionConstraint;
    };

    // One loaded member. Its manifest is parsed eagerly, so a structural error
    // surfaces at workspace-load time rather than at first task invocation.
    struct WorkspaceMember {
        // Verbatim from `workspace.members`, so errors cite what the user typed.
        std::string declaredPath;
        // Member directory, absolute.
        std::string absPath;
        // `<absPath>/cajeta.json`.
        std::string manifestPath;
        // Its `details.name` last segment (or declaredPath's leaf) is the
        // lookup key for the `<member>:<task>` form.
        Manifest manifest;
    };

    struct Workspace {
        // The directory holding the workspace manifest; `${workspace.root}`.
        std::string rootPath;
        // Absolute path of the workspace root's `cajeta.json`.
        std::string manifestPath;
        // The root manifest, kept so callers need not re-load it.
        Manifest rootManifest;
        // Patterns from `workspace.members`, in declaration order.
        std::vector<std::string> memberPatterns;
        // Loaded members, in declaration order.
        std::vector<WorkspaceMember> members;
        // Curated cross-workspace constraints.
        std::vector<WorkspaceSharedDependency> sharedDependencies;
    };

    // The name a member is addressable by from the CLI (`-p <name>`,
    // `<name>:<task>`): its directory leaf, or `details.library()` when that
    // leaf is not a useful name. Stable while the source layout is.
    std::string memberShortName(const WorkspaceMember& m);

    // Parses the root manifest's `workspace` block into the typed model, filling
    // everything but `members` — loadWorkspace also loads those. A missing or
    // malformed block, or an unknown `workspace.*` subfield, is an error.
    llvm::Expected<Workspace> parseWorkspace(const Manifest& root);

    // Parses the workspace root's `cajeta.json` at `rootManifestPath` and loads
    // and validates every member manifest with it.
    llvm::Expected<Workspace> loadWorkspace(
        const std::string& rootManifestPath);

    // Walks up from `startDir` (inclusive) to the nearest `cajeta.json` carrying
    // a `workspace` block, so the CLI works from anywhere inside a workspace.
    std::optional<std::string> discoverWorkspaceRoot(
        const std::string& startDir);

    struct LockfileWorkspaceView {
        std::map<std::string, std::string> recordedChecksums;
    };
    LockfileWorkspaceView extractWorkspaceLockView(
        const struct Lockfile& lf);

    // The member short names needing a rebuild: unrecorded, checksum-changed, or
    // depending on one that does. Clean members are absent, so callers can skip
    // them; a null `lockfileView` rebuilds everything.
    std::set<std::string> membersNeedingRebuild(
        const Workspace& ws,
        const LockfileWorkspaceView* lockfileView);

    // Sorts members into build order — depended-upon first — where A depends on
    // B if A's `settings.dependencies` names B by `library()` segment or short
    // name. A cycle is an error citing both ends.
    llvm::Expected<std::vector<const WorkspaceMember*>>
    topologicallySortMembers(const Workspace& ws);

} // namespace cajeta::buildtool
