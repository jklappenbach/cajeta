// The depth-first walk over a ResolvedGraph and its three renderers. Pure: no
// I/O, no resolution. DepStatus records why a node was not expanded.

#pragma once

#include "cajeta/buildtool/Dependency.h"

#include <string>
#include <vector>

namespace cajeta::buildtool {

    enum class DepStatus { normal, repeated, cycle, opaque, truncated };

    // The word used in JSON and CSV `status`; "" for normal, which both omit.
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
        // Each cycle as the path that closes it, so first and last names are equal.
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

    // The cycles alone; identical to buildDependencyTree(...).cycles by default.
    std::vector<std::vector<std::string>>
    findDependencyCycles(const std::string& rootName, const ResolvedGraph& graph);

    // "a -> b -> a" — the shape the melt detector reads.
    std::string formatCycle(const std::vector<std::string>& path);

    // An indented tree; box-drawing guides, or |-- `-- when ascii.
    std::string renderDepsText(const DepTree& tree, bool ascii = false);
    // One document rooted at a node, validating against deps-output.schema.json.
    std::string renderDepsJson(const DepTree& tree, const std::string& manifestPath);
    // A header plus one row per listed edge, RFC 4180 quoted.
    std::string renderDepsCsv(const DepTree& tree);

} // namespace cajeta::buildtool
