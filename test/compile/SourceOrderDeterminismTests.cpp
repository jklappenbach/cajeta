//
// SourceOrderDeterminismTests — §2.0.7: the same commit must compile to the
// same bytes on any host.
//
// The compiler enumerates a source root with recursive_directory_iterator,
// whose order is filesystem-dependent. It is not the same on two machines, and
// it is not even the same for two checkouts of one repository: a fresh
// `git clone` writes files in a different sequence than a working copy that
// grew file by file over many commits.
//
// Parse order is not cosmetic. It decides synthesized-name tie-breaks,
// first-write-wins archive registry keys, and the point at which an on-demand
// stdlib package (cajeta.math and friends) becomes concrete rather than a
// prescan placeholder. Left unsorted, cajeta-ml compiled clean in its working
// copy and aborted — then, once that was fixed, miscompiled to a binary that
// segfaulted — from a fresh checkout of the identical commit. That failure
// mode is invisible locally and only ever reproduces in CI, which is the worst
// place to discover it.
//
// So the ordering is pinned here rather than left to the filesystem.
//

#include <gtest/gtest.h>

#include "cajeta/compile/Compiler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <list>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Write `name` under `root`, creating parent directories as needed.
void writeSource(const fs::path& root, const std::string& name) {
    fs::path p = root / name;
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << "package test;\npublic final class X {}\n";
}

// A scratch tree that cleans up after itself.
struct TempTree {
    fs::path root;
    explicit TempTree(const std::string& tag) {
        root = fs::temp_directory_path()
             / ("cajeta-order-" + tag + "-"
                + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

// The enumeration is sorted, whatever order the files were created in.
//
// The names are deliberately chosen so that creation order, sorted order, and
// directory-vs-file grouping all disagree — a walker that returns entries as
// the filesystem hands them over will not land on this sequence by luck.
TEST(SourceOrderDeterminismTests, listModulePathsIsSorted) {
    TempTree tree("sorted");
    const std::vector<std::string> created = {
        "zeta/Zulu.cajeta",
        "alpha/Bravo.cajeta",
        "Yankee.cajeta",
        "alpha/Alpha.cajeta",
        "mike/nested/Nested.cajeta",
        "Charlie.cajeta",
        "zeta/Alpha.cajeta",
    };
    for (const auto& name : created) writeSource(tree.root, name);

    std::unique_ptr<std::list<std::string>> paths(
        cajeta::listModulePaths(tree.root.string()));
    ASSERT_NE(paths, nullptr);
    ASSERT_EQ(paths->size(), created.size());

    std::vector<std::string> got(paths->begin(), paths->end());
    std::vector<std::string> want = got;
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want) << "source enumeration must be sorted (§2.0.7) — "
                            "unsorted, the same commit compiles differently "
                            "in a fresh checkout than in a working copy";
}

// Two trees holding identical sources, populated in OPPOSITE creation orders,
// enumerate to the same relative sequence. This is the property that actually
// matters: it is the working-copy-vs-fresh-clone difference, reduced to one
// assertion that cannot depend on how this particular filesystem happens to
// order its entries.
TEST(SourceOrderDeterminismTests, creationOrderDoesNotChangeEnumeration) {
    const std::vector<std::string> names = {
        "grad/GradTape.cajeta",
        "zoo/SmallCnn.cajeta",
        "nn/Module.cajeta",
        "train/BackpropTrainer.cajeta",
        "io/Safetensors.cajeta",
    };

    TempTree forward("fwd");
    for (auto it = names.begin(); it != names.end(); ++it) {
        writeSource(forward.root, *it);
    }
    TempTree reverse("rev");
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        writeSource(reverse.root, *it);
    }

    auto relative = [](const fs::path& root, std::list<std::string>* paths) {
        std::vector<std::string> out;
        for (const auto& p : *paths) {
            out.push_back(fs::relative(p, root).generic_string());
        }
        return out;
    };

    std::unique_ptr<std::list<std::string>> fwd(
        cajeta::listModulePaths(forward.root.string()));
    std::unique_ptr<std::list<std::string>> rev(
        cajeta::listModulePaths(reverse.root.string()));
    ASSERT_NE(fwd, nullptr);
    ASSERT_NE(rev, nullptr);

    EXPECT_EQ(relative(forward.root, fwd.get()),
              relative(reverse.root, rev.get()))
        << "two checkouts of the same sources must enumerate identically";
}
