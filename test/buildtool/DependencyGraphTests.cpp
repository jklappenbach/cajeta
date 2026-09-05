// dependency-tree plan, unit 1 — the resolver keeps its edges (spec §2).
//
// The MVS solver reads every resolved package's manifest sidecar and
// feeds the children back into the fixed point (MvsState::childDeps),
// then flattens to one ResolvedDependency per name. resolveProjectGraph
// returns the same flat list PLUS the edges: the direct roots, each
// package's declared children with the constraint as written, and the
// set of packages that had no sidecar (children unknown, not empty).
//
// Each fixture seeds <home>/.olla through OllaStore::write with a stub
// artifact and a sidecar cajeta.json per package, then resolves with
// homeOverride so no remote is consulted.
//
//   - a chain records every edge, constraint text verbatim       [1.1.1]
//   - a diamond is one node with two edges                       [1.1.2]
//   - a cycle terminates and keeps both edges                    [1.1.3]
//   - a missing sidecar is opaque, not empty                     [1.1.4]
//   - the flat result is unchanged (control)                     [1.1.5]

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/Resolver.h"

#include <gtest/gtest.h>

#include "../PortableEnv.h"
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using cajeta::buildtool::DependencySpec;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::OllaStore;
using cajeta::buildtool::ResolvedGraph;
using cajeta::buildtool::resolveProjectDependencies;
using cajeta::buildtool::resolveProjectGraph;

namespace {

    namespace fs = std::filesystem;

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    fs::path tempDir(const std::string& tag) {
        auto p = fs::temp_directory_path() /
                 ("cajeta-depgraph-" + tag + "-" + std::to_string(::getpid()) +
                  "-" + std::to_string(::rand()));
        fs::create_directories(p);
        return p;
    }

    // Ensure $OLLA_HOME doesn't leak in from the environment so the
    // homeOverride deterministically selects <home>/.olla.
    struct OllaHomeUnset {
        bool had;
        std::string old;
        OllaHomeUnset() {
            const char* v = ::getenv("OLLA_HOME");
            had = v != nullptr;
            if (had) old = v;
            ::unsetenv("OLLA_HOME");
        }
        ~OllaHomeUnset() {
            if (had) ::setenv("OLLA_HOME", old.c_str(), 1);
        }
    };

    using Deps = std::vector<std::pair<std::string, std::string>>;

    // One fixture: a fake home with an olla store, and a project dir.
    struct World {
        OllaHomeUnset guard;
        fs::path home;
        fs::path proj;
        fs::path scratch;
        OllaStore store;
        int counter = 0;

        explicit World(const std::string& tag)
            : home(tempDir(tag + "-home")),
              proj(tempDir(tag + "-proj")),
              scratch(tempDir(tag + "-scratch")),
              store((home / ".olla").string()) {}

        // Seed name@version. `deps` = the sidecar's settings.dependencies
        // (an empty list still writes a sidecar); nullopt = no sidecar.
        void seed(const std::string& name, const std::string& version,
                  std::optional<Deps> deps) {
            auto art = scratch / (name + "-" + std::to_string(counter++) + ".cja");
            { std::ofstream o(art, std::ios::binary); o << "STUB " << name << version; }
            std::optional<std::string> sidecar;
            if (deps) {
                std::string j = "{\"details\":{\"name\":\"" + name +
                                "\",\"version\":\"" + version +
                                "\"},\"settings\":{\"dependencies\":{";
                bool first = true;
                for (const auto& [n, c] : *deps) {
                    if (!first) j += ",";
                    first = false;
                    j += "\"" + n + "\":\"" + c + "\"";
                }
                j += "}}}";
                auto mp = scratch / (name + "-" + std::to_string(counter++) + ".json");
                { std::ofstream o(mp); o << j; }
                sidecar = mp.string();
            }
            auto w = store.write(name, version, art.string(), sidecar);
            ASSERT_TRUE(static_cast<bool>(w)) << errorText(w.takeError());
        }

        // Resolve a consumer manifest whose settings.dependencies is `deps`.
        llvm::Expected<ResolvedGraph> graph(const Deps& deps) {
            auto m = manifest(deps);
            if (!m) return m.takeError();
            return resolveProjectGraph(*m, proj.string(),
                                       /*homeOverride=*/home.string());
        }

        llvm::Expected<cajeta::buildtool::Manifest> manifest(const Deps& deps) {
            std::string j = "{\"details\":{\"name\":\"consumer\",\"version\":\"0.1.0\"},"
                            "\"settings\":{\"dependencies\":{";
            bool first = true;
            for (const auto& [n, c] : deps) {
                if (!first) j += ",";
                first = false;
                j += "\"" + n + "\":\"" + c + "\"";
            }
            j += "}}}";
            return loadManifestString(j);
        }
    };

    std::vector<std::string> names(const std::vector<DependencySpec>& v) {
        std::vector<std::string> out;
        for (const auto& d : v) out.push_back(d.name);
        return out;
    }

    const std::vector<DependencySpec>& childrenOf(const ResolvedGraph& g,
                                                  const std::string& name) {
        static const std::vector<DependencySpec> none;
        auto it = g.children.find(name);
        return it == g.children.end() ? none : it->second;
    }

} // namespace

// 1.1.1 — app → a → b → c. Roots are {a}; each edge carries the constraint
// text its parent wrote, verbatim.
TEST(DependencyGraphTests, chainRecordsEveryEdge) {
    World w("chain");
    w.seed("c.pkg", "1.0.0", Deps{});
    w.seed("b.pkg", "1.0.0", Deps{{"c.pkg", ">=1.0.0"}});
    w.seed("a.pkg", "1.0.0", Deps{{"b.pkg", "1.0.*"}});

    auto g = w.graph({{"a.pkg", "1.0.0"}});
    ASSERT_TRUE(static_cast<bool>(g)) << errorText(g.takeError());

    ASSERT_EQ(g->roots.size(), 1u);
    EXPECT_EQ(g->roots[0].name, "a.pkg");
    EXPECT_EQ(g->roots[0].versionConstraint, "1.0.0");

    ASSERT_EQ(childrenOf(*g, "a.pkg").size(), 1u);
    EXPECT_EQ(childrenOf(*g, "a.pkg")[0].name, "b.pkg");
    EXPECT_EQ(childrenOf(*g, "a.pkg")[0].versionConstraint, "1.0.*");

    ASSERT_EQ(childrenOf(*g, "b.pkg").size(), 1u);
    EXPECT_EQ(childrenOf(*g, "b.pkg")[0].name, "c.pkg");
    EXPECT_EQ(childrenOf(*g, "b.pkg")[0].versionConstraint, ">=1.0.0");

    // c has a sidecar that declares nothing: present and empty, not opaque.
    EXPECT_EQ(g->children.count("c.pkg"), 1u);
    EXPECT_TRUE(childrenOf(*g, "c.pkg").empty());
    EXPECT_TRUE(g->opaque.empty());

    std::vector<std::string> flat;
    for (const auto& p : g->packages) flat.push_back(p.name);
    EXPECT_EQ(flat, (std::vector<std::string>{"a.pkg", "b.pkg", "c.pkg"}));
}

// 1.1.2 — a → d and b → d with different constraints: one package d in the
// flat list, two edges each carrying its own parent's constraint.
TEST(DependencyGraphTests, diamondIsOneNodeWithTwoEdges) {
    World w("diamond");
    w.seed("d.pkg", "1.0.0", Deps{});
    w.seed("a.pkg", "1.0.0", Deps{{"d.pkg", "1.0.*"}});
    w.seed("b.pkg", "1.0.0", Deps{{"d.pkg", ">=1.0.0"}});

    auto g = w.graph({{"a.pkg", "1.0.0"}, {"b.pkg", "1.0.0"}});
    ASSERT_TRUE(static_cast<bool>(g)) << errorText(g.takeError());

    int dCount = 0;
    for (const auto& p : g->packages) if (p.name == "d.pkg") ++dCount;
    EXPECT_EQ(dCount, 1);

    EXPECT_EQ(names(g->roots), (std::vector<std::string>{"a.pkg", "b.pkg"}));
    ASSERT_EQ(childrenOf(*g, "a.pkg").size(), 1u);
    ASSERT_EQ(childrenOf(*g, "b.pkg").size(), 1u);
    EXPECT_EQ(childrenOf(*g, "a.pkg")[0].name, "d.pkg");
    EXPECT_EQ(childrenOf(*g, "a.pkg")[0].versionConstraint, "1.0.*");
    EXPECT_EQ(childrenOf(*g, "b.pkg")[0].name, "d.pkg");
    EXPECT_EQ(childrenOf(*g, "b.pkg")[0].versionConstraint, ">=1.0.0");
}

// 1.1.3 — a → b and b → a. Resolution returns (the fixed point tolerates
// the loop) and both edges are present. Guards spec §4.7 at the graph layer.
TEST(DependencyGraphTests, cycleTerminatesAndKeepsBothEdges) {
    World w("cycle");
    w.seed("a.pkg", "1.0.0", Deps{{"b.pkg", "1.0.0"}});
    w.seed("b.pkg", "1.0.0", Deps{{"a.pkg", "1.0.0"}});

    auto g = w.graph({{"a.pkg", "1.0.0"}});
    ASSERT_TRUE(static_cast<bool>(g)) << errorText(g.takeError());

    EXPECT_EQ(names(childrenOf(*g, "a.pkg")), (std::vector<std::string>{"b.pkg"}));
    EXPECT_EQ(names(childrenOf(*g, "b.pkg")), (std::vector<std::string>{"a.pkg"}));
    EXPECT_EQ(g->packages.size(), 2u);
}

// 1.1.4 — a package written with no sidecar is in `opaque` and has no
// `children` entry: its children are unknown, which is not the same as none.
TEST(DependencyGraphTests, missingSidecarIsOpaqueNotEmpty) {
    World w("opaque");
    w.seed("x.pkg", "1.0.0", std::nullopt);

    auto g = w.graph({{"x.pkg", "1.0.0"}});
    ASSERT_TRUE(static_cast<bool>(g)) << errorText(g.takeError());

    EXPECT_EQ(g->opaque.count("x.pkg"), 1u);
    EXPECT_EQ(g->children.count("x.pkg"), 0u);
    ASSERT_EQ(g->packages.size(), 1u);
    EXPECT_EQ(g->packages[0].name, "x.pkg");
}

// 1.1.5 — the control. On the diamond, the flat entry point returns the
// same names, versions and order as the graph's package list.
TEST(DependencyGraphTests, flatResultIsUnchanged) {
    World w("flat");
    w.seed("d.pkg", "1.0.0", Deps{});
    w.seed("a.pkg", "1.0.0", Deps{{"d.pkg", "1.0.*"}});
    w.seed("b.pkg", "1.0.0", Deps{{"d.pkg", ">=1.0.0"}});

    Deps consumer{{"a.pkg", "1.0.0"}, {"b.pkg", "1.0.0"}};
    auto m = w.manifest(consumer);
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());

    auto flat = resolveProjectDependencies(*m, w.proj.string(), w.home.string());
    ASSERT_TRUE(static_cast<bool>(flat)) << errorText(flat.takeError());
    auto g = w.graph(consumer);
    ASSERT_TRUE(static_cast<bool>(g)) << errorText(g.takeError());

    ASSERT_EQ(flat->size(), g->packages.size());
    for (size_t i = 0; i < flat->size(); ++i) {
        EXPECT_EQ((*flat)[i].name, g->packages[i].name);
        EXPECT_EQ((*flat)[i].version, g->packages[i].version);
        EXPECT_EQ((*flat)[i].resolvedFromRepo, g->packages[i].resolvedFromRepo);
        EXPECT_EQ((*flat)[i].sha256, g->packages[i].sha256);
    }
    EXPECT_EQ(flat->size(), 3u);
}
