// DependencyTree — the walk over a ResolvedGraph and its three renderers
// (dependency-tree spec §3, §4, §5). Everything here is pure: no I/O, no
// resolution. `cajeta deps` resolves, calls buildDependencyTree, and prints
// one of the render* strings; `build` calls findDependencyCycles to warn.
//
// The walk is depth-first from the project root. A child already on the
// path from the root closes a CYCLE and is not followed (§4.1); a child
// whose subtree was already printed is REPEATED and not expanded again
// unless dedupe is off (§3.2); a package with no manifest sidecar is
// OPAQUE — its children are unknown, not none (§3.4); a node cut by the
// depth limit that had children is TRUNCATED (§3.3). Children are listed
// in name order at every level (§3.5).

#pragma once

#include "cajeta/buildtool/Dependency.h"

#include <string>
#include <vector>

namespace cajeta::buildtool {

    enum class DepStatus { normal, repeated, cycle, opaque, truncated };

    // The word used in JSON `status` and the CSV `status` column; "" for
    // normal (JSON omits the key, CSV leaves the field empty).
    const char* depStatusWord(DepStatus s);

    struct DepNode {
        std::string name;
        std::string version;     // resolved; empty only if unresolved
        std::string requested;   // the constraint the PARENT declared; "" on the root
        std::string repository;  // repo that supplied the artifact ("olla" = local store)
        std::string checksum;    // "sha256:<hex>"
        DepStatus status = DepStatus::normal;
        std::vector<DepNode> children;
    };

    struct DepTree {
        DepNode root;  // the project; root.children are the direct deps
        // Each cycle as the path that closes it: first and last names are
        // equal (`a b a`; a self-loop is `a a`; a dependency named like the
        // project is `<project> <project>`). Walk order, deduplicated.
        std::vector<std::vector<std::string>> cycles;
    };

    struct DepTreeOptions {
        int depth = -1;      // -1 = unlimited; 0 = root alone; 1 = direct deps only
        bool dedupe = true;  // expand a repeated subtree once (§3.2)
    };

    DepTree buildDependencyTree(const std::string& rootName,
                                const std::string& rootVersion,
                                const ResolvedGraph& graph,
                                const DepTreeOptions& options = {});

    // The cycles alone, for callers that do not render (`build`'s warning,
    // §8.5). Identical to buildDependencyTree(...).cycles with default
    // options, so the two callers cannot disagree.
    std::vector<std::vector<std::string>>
    findDependencyCycles(const std::string& rootName, const ResolvedGraph& graph);

    // "a -> b -> a" — the shape the melt detector uses (§4.4).
    std::string formatCycle(const std::vector<std::string>& path);

    // §5.1 — indented tree; box-drawing guides, or |-- `-- when ascii.
    std::string renderDepsText(const DepTree& tree, bool ascii = false);
    // §5.2 — one document, root is a node; validates against
    // specs/schemas/deps-output.schema.json. Pretty-printed, 2 spaces.
    std::string renderDepsJson(const DepTree& tree, const std::string& manifestPath);
    // §5.3 — header + one row per listed edge, RFC 4180 quoting.
    std::string renderDepsCsv(const DepTree& tree);

} // namespace cajeta::buildtool
