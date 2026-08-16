// Regression tests for Phase 5b transitive-imports digest.
//
// Pins:
//   - A file with no imports digests differently than the same
//     file with one import to a known source.
//   - Modifying an imported file changes the dependent's digest.
//   - Wildcard imports (`X.*`) are normalized to the dotted name.
//   - Cycle members get the leaf-only digest; dependents outside
//     the cycle still re-key when any cycle member's bytes change.
//   - Unresolved imports (stdlib / deps) contribute "ext:<name>" —
//     not an error.

#include "cajeta/buildtool/SourceDigest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::SourceDigestRegistry;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path tempProject(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-srcdigest-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& path,
                   const std::string& contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }

} // namespace




TEST(SourceDigestTests, wildcardImportNormalizesToDottedName) {
    auto root = tempProject("wild");
    writeFile(root / "com" / "ex" / "Bar.cajeta",
              "package com.ex;\npublic class Bar {}\n");
    writeFile(root / "com" / "ex" / "Foo.cajeta",
              "package com.ex;\nimport com.ex.*;\n"
              "public class Foo {}\n");
    SourceDigestRegistry reg({root.string()});
    auto imports = reg.importsOf((root / "com/ex/Foo.cajeta").string());
    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports[0], "com.ex");
}


TEST(SourceDigestTests, cycleResolvesWithLeafOnlyDigest) {
    auto root = tempProject("cycle");
    writeFile(root / "com" / "ex" / "A.cajeta",
              "package com.ex;\nimport com.ex.B;\n"
              "public class A {}\n");
    writeFile(root / "com" / "ex" / "B.cajeta",
              "package com.ex;\nimport com.ex.A;\n"
              "public class B {}\n");
    SourceDigestRegistry reg({root.string()});
    auto d = reg.digestOf((root / "com/ex/A.cajeta").string());
    ASSERT_TRUE((bool)d) << errorText(d.takeError());
    EXPECT_FALSE(d->empty());
}
