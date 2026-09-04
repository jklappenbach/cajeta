// dependency-tree plan, unit 2 — the walk and the renderers (spec §3, §4,
// §5.1–§5.3). Hand-built ResolvedGraph values; no store, no I/O.
//
//   - a chain nests under its parents                        [2.1.1]
//   - a diamond lists both parents, expands once             [2.1.2]
//   - a cycle is marked, reported, and terminates            [2.1.3]
//   - an acyclic graph is unaffected by detection (control)  [2.1.4]
//   - everyone depends on everyone: still terminates         [2.1.5]
//   - --depth truncates                                      [2.1.6]
//   - children sorted by name                                [2.1.7]
//   - opaque is marked                                       [2.1.8]
//   - JSON golden shape (parsed back, structural)            [2.1.9]
//   - CSV quotes a comma constraint                          [2.1.10]
//   - text golden, unicode and ascii                         [2.1.11]
//   - empty project                                          [2.1.12]

#include "cajeta/buildtool/DependencyTree.h"

#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace cajeta::buildtool;

namespace {

    using Edges = std::vector<std::pair<std::string, std::string>>;
    const std::string kSha =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // Build a graph: `roots` are the direct deps (name → constraint);
    // `edges[name]` are that package's declared children; every name seen
    // anywhere becomes a resolved package at 1.0.0 from "olla", unless it
    // is in `opaque` (then it has no children entry at all).
    ResolvedGraph graphOf(const Edges& roots,
                          const std::map<std::string, Edges>& edges,
                          const std::set<std::string>& opaque = {}) {
        ResolvedGraph g;
        std::set<std::string> seen;
        auto pkg = [&](const std::string& n) {
            if (!seen.insert(n).second) return;
            ResolvedDependency r;
            r.name = n; r.version = "1.0.0"; r.resolvedFromRepo = "olla";
            r.artifactPath = "/cache/" + n + ".cja"; r.sha256 = kSha;
            g.packages.push_back(r);
        };
        for (const auto& [n, c] : roots) {
            g.roots.push_back(DependencySpec{n, c, std::nullopt});
            pkg(n);
        }
        for (const auto& [n, kids] : edges) {
            pkg(n);
            auto& out = g.children[n];
            for (const auto& [k, c] : kids) {
                out.push_back(DependencySpec{k, c, std::nullopt});
                pkg(k);
            }
        }
        for (const auto& n : opaque) { pkg(n); g.opaque.insert(n); g.children.erase(n); }
        // Any package not opaque and without an edges entry is a leaf with
        // a sidecar (children present, empty).
        for (const auto& p : g.packages)
            if (!g.opaque.count(p.name) && !g.children.count(p.name))
                g.children[p.name] = {};
        return g;
    }

    // The diamond: app → a, app → b, a → d ("1.0.*"), b → d (">=1.0.0").
    ResolvedGraph diamond() {
        return graphOf({{"a", "1.0.0"}, {"b", "1.0.0"}},
                       {{"a", {{"d", "1.0.*"}}}, {"b", {{"d", ">=1.0.0"}}}});
    }

    const DepNode& child(const DepNode& n, size_t i) {
        EXPECT_LT(i, n.children.size());
        return n.children.at(i);
    }

    llvm::json::Value parseOrFail(const std::string& s) {
        auto v = llvm::json::parse(s);
        EXPECT_TRUE(static_cast<bool>(v))
            << "emission is not valid JSON: "
            << (v ? "" : llvm::toString(v.takeError()));
        return v ? std::move(*v) : llvm::json::Value(nullptr);
    }

    std::set<std::string> keysOf(const llvm::json::Object& o) {
        std::set<std::string> k;
        for (const auto& kv : o) k.insert(kv.first.str());
        return k;
    }

    std::vector<std::string> lines(const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size()) {
            size_t nl = s.find('\n', start);
            if (nl == std::string::npos) nl = s.size();
            out.push_back(s.substr(start, nl - start));
            start = nl + 1;
        }
        return out;
    }

} // namespace

// 2.1.1 — app → a → b → c: b under a, c under b (§3.1).
TEST(DependencyTreeTests, chainNestsUnderParents) {
    auto g = graphOf({{"a", "1.0.0"}},
                     {{"a", {{"b", "1.0.*"}}}, {"b", {{"c", ">=1.0.0"}}}});
    auto t = buildDependencyTree("app", "0.1.0", g);
    EXPECT_EQ(t.root.name, "app");
    EXPECT_EQ(t.root.version, "0.1.0");
    ASSERT_EQ(t.root.children.size(), 1u);
    const auto& a = child(t.root, 0);
    EXPECT_EQ(a.name, "a");
    EXPECT_EQ(a.requested, "1.0.0");
    const auto& b = child(a, 0);
    EXPECT_EQ(b.name, "b");
    EXPECT_EQ(b.requested, "1.0.*");
    const auto& c = child(b, 0);
    EXPECT_EQ(c.name, "c");
    EXPECT_EQ(c.requested, ">=1.0.0");
    EXPECT_TRUE(c.children.empty());
    EXPECT_EQ(c.status, DepStatus::normal);

    auto text = renderDepsText(t, /*ascii=*/true);
    EXPECT_EQ(text, "app 0.1.0\n`-- a 1.0.0\n    `-- b 1.0.0\n        `-- c 1.0.0\n");
}

// 2.1.2 — d under a and under b; the second is `repeated` with no
// children and its own parent's constraint; dedupe=false expands both
// (§3.2, §2.5).
TEST(DependencyTreeTests, diamondListsBothParentsExpandsOnce) {
    auto t = buildDependencyTree("app", "1.0.0", diamond());
    ASSERT_EQ(t.root.children.size(), 2u);
    const auto& a = child(t.root, 0);
    const auto& b = child(t.root, 1);
    const auto& dUnderA = child(a, 0);
    const auto& dUnderB = child(b, 0);
    EXPECT_EQ(dUnderA.name, "d");
    EXPECT_EQ(dUnderB.name, "d");
    EXPECT_EQ(dUnderA.requested, "1.0.*");
    EXPECT_EQ(dUnderB.requested, ">=1.0.0");
    EXPECT_EQ(dUnderA.status, DepStatus::normal);
    EXPECT_EQ(dUnderB.status, DepStatus::repeated);
    EXPECT_TRUE(dUnderB.children.empty());

    DepTreeOptions all;
    all.dedupe = false;
    auto full = buildDependencyTree("app", "1.0.0", diamond(), all);
    EXPECT_EQ(child(child(full.root, 1), 0).status, DepStatus::normal);
}

// 2.1.3 — app → a → b → a: the `a` under `b` is `cycle`, and the cycles
// list has the closing path. A self-loop and a dependency named like the
// project are cycles too (§4.1–§4.4).
TEST(DependencyTreeTests, cycleIsMarkedReportedAndTerminates) {
    auto g = graphOf({{"a", "1.0.0"}},
                     {{"a", {{"b", "1.0.0"}}}, {"b", {{"a", "1.0.0"}}}});
    auto t = buildDependencyTree("app", "1.0.0", g);
    const auto& a = child(t.root, 0);
    const auto& b = child(a, 0);
    const auto& back = child(b, 0);
    EXPECT_EQ(back.name, "a");
    EXPECT_EQ(back.status, DepStatus::cycle);
    EXPECT_TRUE(back.children.empty());
    ASSERT_EQ(t.cycles.size(), 1u);
    EXPECT_EQ(t.cycles[0], (std::vector<std::string>{"a", "b", "a"}));
    EXPECT_EQ(formatCycle(t.cycles[0]), "a -> b -> a");

    auto self = buildDependencyTree("app", "1.0.0",
        graphOf({{"a", "1.0.0"}}, {{"a", {{"a", "1.0.0"}}}}));
    ASSERT_EQ(self.cycles.size(), 1u);
    EXPECT_EQ(self.cycles[0], (std::vector<std::string>{"a", "a"}));
    EXPECT_EQ(child(child(self.root, 0), 0).status, DepStatus::cycle);

    auto viaRoot = buildDependencyTree("app", "1.0.0",
        graphOf({{"app", "0.9.0"}}, {}));
    ASSERT_EQ(viaRoot.cycles.size(), 1u);
    EXPECT_EQ(viaRoot.cycles[0], (std::vector<std::string>{"app", "app"}));
    EXPECT_EQ(child(viaRoot.root, 0).status, DepStatus::cycle);

    // findDependencyCycles is the same detector without the tree (§8.5).
    EXPECT_EQ(findDependencyCycles("app", g), t.cycles);
}

// 2.1.4 — the control (§4.6): on the diamond, no cycles and every node
// present.
TEST(DependencyTreeTests, acyclicGraphIsUnaffectedByDetection) {
    auto t = buildDependencyTree("app", "1.0.0", diamond());
    EXPECT_TRUE(t.cycles.empty());
    EXPECT_TRUE(findDependencyCycles("app", diamond()).empty());
    std::set<std::string> seen;
    std::vector<const DepNode*> stack{&t.root};
    while (!stack.empty()) {
        const DepNode* n = stack.back(); stack.pop_back();
        seen.insert(n->name);
        EXPECT_NE(n->status, DepStatus::cycle);
        for (const auto& c : n->children) stack.push_back(&c);
    }
    EXPECT_EQ(seen, (std::set<std::string>{"app", "a", "b", "d"}));
}

// 2.1.5 — four packages, fully connected: the walk returns, every edge
// appears exactly once (expanded or cycle), cycles is non-empty (§4.7).
TEST(DependencyTreeTests, everyoneDependsOnEveryone) {
    std::vector<std::string> names{"p", "q", "r", "s"};
    Edges roots;
    std::map<std::string, Edges> edges;
    for (const auto& n : names) {
        roots.push_back({n, "1.0.0"});
        for (const auto& m : names) edges[n].push_back({m, "1.0.0"});
    }
    auto t = buildDependencyTree("app", "1.0.0", graphOf(roots, edges));
    EXPECT_FALSE(t.cycles.empty());

    // Count edges (parent,child) in the tree; each declared edge is listed
    // once because dedupe expands each package once.
    std::multiset<std::string> listed;
    std::vector<const DepNode*> stack{&t.root};
    while (!stack.empty()) {
        const DepNode* n = stack.back(); stack.pop_back();
        for (const auto& c : n->children) {
            listed.insert(n->name + ">" + c.name);
            stack.push_back(&c);
        }
    }
    for (const auto& n : names)
        for (const auto& m : names)
            EXPECT_EQ(listed.count(n + ">" + m), 1u) << n << ">" << m;
    EXPECT_EQ(listed.size(), 4u + 16u);  // 4 root edges + 16 declared

    // Without dedupe it still terminates.
    DepTreeOptions all; all.dedupe = false;
    auto full = buildDependencyTree("app", "1.0.0", graphOf(roots, edges), all);
    EXPECT_FALSE(full.cycles.empty());
}

// 2.1.6 — depth 1 lists direct deps only and marks those with children
// `truncated`; depth 0 is the root alone (§3.3).
TEST(DependencyTreeTests, depthTruncates) {
    DepTreeOptions one; one.depth = 1;
    auto t = buildDependencyTree("app", "1.0.0", diamond(), one);
    ASSERT_EQ(t.root.children.size(), 2u);
    EXPECT_EQ(child(t.root, 0).status, DepStatus::truncated);
    EXPECT_TRUE(child(t.root, 0).children.empty());
    EXPECT_EQ(child(t.root, 1).status, DepStatus::truncated);

    // A leaf at the depth limit is NOT truncated: it had nothing to cut.
    auto leaf = buildDependencyTree("app", "1.0.0",
        graphOf({{"a", "1.0.0"}}, {}), one);
    EXPECT_EQ(child(leaf.root, 0).status, DepStatus::normal);

    DepTreeOptions zero; zero.depth = 0;
    auto r = buildDependencyTree("app", "1.0.0", diamond(), zero);
    EXPECT_TRUE(r.root.children.empty());
    EXPECT_EQ(renderDepsText(r, true), "app 1.0.0\n");
}

// 2.1.7 — declared z, a, m; printed a, m, z, at every level (§3.5).
TEST(DependencyTreeTests, childrenSortedByName) {
    auto g = graphOf({{"z", "1.0.0"}, {"a", "1.0.0"}, {"m", "1.0.0"}},
                     {{"m", {{"y", "1.0.0"}, {"b", "1.0.0"}}}});
    auto t = buildDependencyTree("app", "1.0.0", g);
    ASSERT_EQ(t.root.children.size(), 3u);
    EXPECT_EQ(child(t.root, 0).name, "a");
    EXPECT_EQ(child(t.root, 1).name, "m");
    EXPECT_EQ(child(t.root, 2).name, "z");
    EXPECT_EQ(child(child(t.root, 1), 0).name, "b");
    EXPECT_EQ(child(child(t.root, 1), 1).name, "y");
}

// 2.1.8 — a package with no sidecar is `opaque` (§3.4).
TEST(DependencyTreeTests, opaqueIsMarked) {
    auto g = graphOf({{"x", "1.0.0"}}, {}, {"x"});
    auto t = buildDependencyTree("app", "1.0.0", g);
    EXPECT_EQ(child(t.root, 0).status, DepStatus::opaque);
    EXPECT_EQ(renderDepsText(t, true), "app 1.0.0\n`-- x 1.0.0 (no manifest)\n");
}

// 2.1.9 — JSON: parse back; key sets per §5.2.1–§5.2.3; `status` absent on
// normal nodes; two-space indentation (§5.2.4).
TEST(DependencyTreeTests, jsonGoldenShape) {
    auto g = graphOf({{"a", "1.0.0"}},
                     {{"a", {{"b", "1.0.0"}}}, {"b", {{"a", "1.0.0"}}}});
    auto t = buildDependencyTree("app", "0.1.0", g);
    std::string s = renderDepsJson(t, "/abs/path/cajeta.json");
    auto v = parseOrFail(s);
    auto* root = v.getAsObject();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(keysOf(*root), (std::set<std::string>{
        "name", "version", "manifest", "dependencies", "cycles"}));
    EXPECT_EQ(root->getString("name").value_or(""), "app");
    EXPECT_EQ(root->getString("version").value_or(""), "0.1.0");
    EXPECT_EQ(root->getString("manifest").value_or(""), "/abs/path/cajeta.json");

    auto* deps = root->getArray("dependencies");
    ASSERT_NE(deps, nullptr);
    ASSERT_EQ(deps->size(), 1u);
    auto* a = (*deps)[0].getAsObject();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(keysOf(*a), (std::set<std::string>{
        "name", "version", "requested", "repository", "checksum", "dependencies"}));
    EXPECT_EQ(a->getString("requested").value_or(""), "1.0.0");
    EXPECT_EQ(a->getString("repository").value_or(""), "olla");
    EXPECT_EQ(a->getString("checksum").value_or(""), kSha);

    auto* b = (*a->getArray("dependencies"))[0].getAsObject();
    ASSERT_NE(b, nullptr);
    auto* back = (*b->getArray("dependencies"))[0].getAsObject();
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->getString("status").value_or(""), "cycle");
    ASSERT_NE(back->getArray("dependencies"), nullptr);
    EXPECT_TRUE(back->getArray("dependencies")->empty());

    auto* cycles = root->getArray("cycles");
    ASSERT_NE(cycles, nullptr);
    ASSERT_EQ(cycles->size(), 1u);
    auto* c0 = (*cycles)[0].getAsArray();
    ASSERT_NE(c0, nullptr);
    ASSERT_EQ(c0->size(), 3u);
    EXPECT_EQ((*c0)[0].getAsString().value_or(""), "a");
    EXPECT_EQ((*c0)[2].getAsString().value_or(""), "a");

    // Pretty-printed, two spaces.
    EXPECT_NE(s.find("\n  \""), std::string::npos) << s;
}

// 2.1.10 — CSV: header exact, a comma constraint quoted, an inner quote
// doubled, row count = listed edges (§5.3).
TEST(DependencyTreeTests, csvQuotesCommaConstraint) {
    auto g = graphOf({{"a", ">=1.0, <2.0"}, {"b", "1.0.0"}},
                     {{"a", {{"d", "say \"hi\""}}}, {"b", {{"d", "1.0.*"}}}});
    auto t = buildDependencyTree("app", "1.0.0", g);
    auto rows = lines(renderDepsCsv(t));
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0], "parent,name,version,requested,repository,checksum,depth,status");
    EXPECT_EQ(rows[1], "app,a,1.0.0,\">=1.0, <2.0\",olla," + kSha + ",1,");
    EXPECT_EQ(rows[2], "a,d,1.0.0,\"say \"\"hi\"\"\",olla," + kSha + ",2,");
    EXPECT_EQ(rows[3], "app,b,1.0.0,1.0.0,olla," + kSha + ",1,");
    EXPECT_EQ(rows[4], "b,d,1.0.0,1.0.*,olla," + kSha + ",2,repeated");
}

// 2.1.11 — text goldens for the diamond, both guide sets (§5.1).
TEST(DependencyTreeTests, textGoldenUnicodeAndAscii) {
    auto t = buildDependencyTree("app", "1.0.0", diamond());
    EXPECT_EQ(renderDepsText(t, /*ascii=*/false),
              "app 1.0.0\n"
              "├── a 1.0.0\n"
              "│   └── d 1.0.0\n"
              "└── b 1.0.0\n"
              "    └── d 1.0.0 (*)\n");
    EXPECT_EQ(renderDepsText(t, /*ascii=*/true),
              "app 1.0.0\n"
              "|-- a 1.0.0\n"
              "|   `-- d 1.0.0\n"
              "`-- b 1.0.0\n"
              "    `-- d 1.0.0 (*)\n");

    // Every status word has its marker.
    auto g = graphOf({{"a", "1.0.0"}, {"x", "1.0.0"}},
                     {{"a", {{"a", "1.0.0"}}}}, {"x"});
    auto s = buildDependencyTree("app", "1.0.0", g);
    EXPECT_EQ(renderDepsText(s, true),
              "app 1.0.0\n"
              "|-- a 1.0.0\n"
              "|   `-- a 1.0.0 (cycle)\n"
              "`-- x 1.0.0 (no manifest)\n");
    DepTreeOptions one; one.depth = 1;
    auto cut = buildDependencyTree("app", "1.0.0", diamond(), one);
    EXPECT_EQ(renderDepsText(cut, true),
              "app 1.0.0\n|-- a 1.0.0 (...)\n`-- b 1.0.0 (...)\n");
}

// 2.1.12 — no dependencies: the root alone in all three formats (§3.6).
TEST(DependencyTreeTests, emptyProject) {
    ResolvedGraph g;
    auto t = buildDependencyTree("app", "2.0.0", g);
    EXPECT_TRUE(t.root.children.empty());
    EXPECT_TRUE(t.cycles.empty());
    EXPECT_EQ(renderDepsText(t, false), "app 2.0.0\n");
    EXPECT_EQ(renderDepsCsv(t),
              "parent,name,version,requested,repository,checksum,depth,status\n");
    auto v = parseOrFail(renderDepsJson(t, "/p/cajeta.json"));
    auto* root = v.getAsObject();
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->getArray("dependencies")->empty());
    EXPECT_TRUE(root->getArray("cycles")->empty());
}
