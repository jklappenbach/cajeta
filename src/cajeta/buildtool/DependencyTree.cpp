#include "cajeta/buildtool/DependencyTree.h"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cajeta::buildtool {

    const char* depStatusWord(DepStatus s) {
        switch (s) {
            case DepStatus::normal:    return "";
            case DepStatus::repeated:  return "repeated";
            case DepStatus::cycle:     return "cycle";
            case DepStatus::opaque:    return "opaque";
            case DepStatus::truncated: return "truncated";
        }
        return "";
    }

    std::string formatCycle(const std::vector<std::string>& path) {
        std::string out;
        for (size_t i = 0; i < path.size(); ++i) {
            if (i) out += " -> ";
            out += path[i];
        }
        return out;
    }

    namespace {

        struct Walker {
            const ResolvedGraph& g;
            const DepTreeOptions& opt;
            std::unordered_map<std::string, const ResolvedDependency*> byName;
            std::vector<std::string> path;        // root first
            std::unordered_set<std::string> onPath;
            std::unordered_set<std::string> expanded;
            std::vector<std::vector<std::string>> cycles;
            std::set<std::string> cycleKeys;

            Walker(const ResolvedGraph& graph, const DepTreeOptions& options)
                : g(graph), opt(options) {
                for (const auto& p : g.packages) byName[p.name] = &p;
            }

            static std::vector<DependencySpec>
            sorted(const std::vector<DependencySpec>& in) {
                auto out = in;
                std::stable_sort(out.begin(), out.end(),
                    [](const DependencySpec& a, const DependencySpec& b) {
                        return a.name < b.name;
                    });
                return out;
            }

            DepNode make(const DependencySpec& spec) const {
                DepNode n;
                n.name = spec.name;
                n.requested = spec.versionConstraint;
                auto it = byName.find(spec.name);
                if (it != byName.end()) {
                    n.version = it->second->version;
                    n.repository = it->second->resolvedFromRepo;
                    n.checksum = it->second->sha256;
                }
                return n;
            }

            void recordCycle(const std::string& name) {
                auto it = std::find(path.begin(), path.end(), name);
                std::vector<std::string> cyc(it, path.end());
                cyc.push_back(name);
                if (cycleKeys.insert(formatCycle(cyc)).second)
                    cycles.push_back(std::move(cyc));
            }

            // List `kids` under `node`, which sits at `level` (root = 0).
            // Only called when level+1 is within the depth limit.
            void visit(DepNode& node, const std::vector<DependencySpec>& kids,
                       int level) {
                const int childLevel = level + 1;
                for (const auto& spec : sorted(kids)) {
                    DepNode c = make(spec);
                    if (onPath.count(c.name)) {
                        c.status = DepStatus::cycle;            // §4.1
                        recordCycle(c.name);
                    } else if (opt.dedupe && expanded.count(c.name)) {
                        c.status = DepStatus::repeated;         // §3.2
                    } else if (g.opaque.count(c.name)) {
                        c.status = DepStatus::opaque;           // §3.4
                    } else {
                        static const std::vector<DependencySpec> none;
                        auto it = g.children.find(c.name);
                        const auto& grandkids =
                            it == g.children.end() ? none : it->second;
                        if (opt.depth >= 0 && childLevel + 1 > opt.depth) {
                            if (!grandkids.empty())
                                c.status = DepStatus::truncated; // §3.3
                        } else {
                            expanded.insert(c.name);
                            path.push_back(c.name);
                            onPath.insert(c.name);
                            visit(c, grandkids, childLevel);
                            onPath.erase(c.name);
                            path.pop_back();
                        }
                    }
                    node.children.push_back(std::move(c));
                }
            }
        };

    } // namespace

    DepTree buildDependencyTree(const std::string& rootName,
                                const std::string& rootVersion,
                                const ResolvedGraph& graph,
                                const DepTreeOptions& options) {
        DepTree tree;
        tree.root.name = rootName;
        tree.root.version = rootVersion;
        Walker w(graph, options);
        // The root is on the path, so a dependency named like the project
        // closes a cycle through it (§4.2).
        w.path.push_back(rootName);
        w.onPath.insert(rootName);
        if (!(options.depth >= 0 && 1 > options.depth))
            w.visit(tree.root, graph.roots, 0);
        tree.cycles = std::move(w.cycles);
        return tree;
    }

    std::vector<std::vector<std::string>>
    findDependencyCycles(const std::string& rootName, const ResolvedGraph& graph) {
        return buildDependencyTree(rootName, "", graph).cycles;
    }

    // ---- text -------------------------------------------------------------

    namespace {
        const char* marker(DepStatus s) {
            switch (s) {
                case DepStatus::normal:    return "";
                case DepStatus::repeated:  return " (*)";
                case DepStatus::cycle:     return " (cycle)";
                case DepStatus::opaque:    return " (no manifest)";
                case DepStatus::truncated: return " (...)";
            }
            return "";
        }

        std::string label(const DepNode& n) {
            std::string s = n.name;
            if (!n.version.empty()) s += " " + n.version;
            return s;
        }

        void renderTextChildren(const DepNode& node, const std::string& prefix,
                                bool ascii, std::string& out) {
            const char* tee  = ascii ? "|-- " : "├── ";
            const char* last = ascii ? "`-- " : "└── ";
            const char* bar  = ascii ? "|   " : "│   ";
            const char* gap  = "    ";
            for (size_t i = 0; i < node.children.size(); ++i) {
                const DepNode& c = node.children[i];
                const bool isLast = i + 1 == node.children.size();
                out += prefix + (isLast ? last : tee) + label(c) + marker(c.status) + "\n";
                renderTextChildren(c, prefix + (isLast ? gap : bar), ascii, out);
            }
        }
    } // namespace

    std::string renderDepsText(const DepTree& tree, bool ascii) {
        std::string out = label(tree.root) + "\n";
        renderTextChildren(tree.root, "", ascii, out);
        return out;
    }

    // ---- json -------------------------------------------------------------

    namespace {
        llvm::json::Object nodeJson(const DepNode& n) {
            llvm::json::Object o;
            o["name"] = n.name;
            o["version"] = n.version;
            o["requested"] = n.requested;
            o["repository"] = n.repository;
            o["checksum"] = n.checksum;
            if (n.status != DepStatus::normal) o["status"] = depStatusWord(n.status);
            llvm::json::Array kids;
            for (const auto& c : n.children) kids.push_back(nodeJson(c));
            o["dependencies"] = std::move(kids);
            return o;
        }
    } // namespace

    std::string renderDepsJson(const DepTree& tree, const std::string& manifestPath) {
        llvm::json::Object root;
        root["name"] = tree.root.name;
        root["version"] = tree.root.version;
        root["manifest"] = manifestPath;
        llvm::json::Array deps;
        for (const auto& c : tree.root.children) deps.push_back(nodeJson(c));
        root["dependencies"] = std::move(deps);
        llvm::json::Array cycles;
        for (const auto& cyc : tree.cycles) {
            llvm::json::Array path;
            for (const auto& n : cyc) path.push_back(n);
            cycles.push_back(std::move(path));
        }
        root["cycles"] = std::move(cycles);

        std::string out;
        llvm::raw_string_ostream os(out);
        os << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
        return os.str();
    }

    // ---- csv --------------------------------------------------------------

    namespace {
        std::string csvField(const std::string& s) {
            if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
            std::string q = "\"";
            for (char ch : s) {
                if (ch == '"') q += '"';
                q += ch;
            }
            q += "\"";
            return q;
        }

        void renderCsvRows(const DepNode& parent, int depth, std::string& out) {
            for (const auto& c : parent.children) {
                out += csvField(parent.name) + "," + csvField(c.name) + "," +
                       csvField(c.version) + "," + csvField(c.requested) + "," +
                       csvField(c.repository) + "," + csvField(c.checksum) + "," +
                       std::to_string(depth) + "," + depStatusWord(c.status) + "\n";
                renderCsvRows(c, depth + 1, out);
            }
        }
    } // namespace

    std::string renderDepsCsv(const DepTree& tree) {
        std::string out =
            "parent,name,version,requested,repository,checksum,depth,status\n";
        renderCsvRows(tree.root, 1, out);
        return out;
    }

} // namespace cajeta::buildtool
