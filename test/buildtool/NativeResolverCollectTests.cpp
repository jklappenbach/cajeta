// Native-deps unit 4 — requirement model + transitive collection.
// See native-deps-plan.md unit 4, spec §4.1.

#include "cajeta/buildtool/NativeResolver.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>
#include <vector>

using cajeta::CajetaArchive;
using cajeta::buildtool::collectNativeRequirements;

namespace {
std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
// Build an in-memory archive carrying the given native-meta json.
CajetaArchive archiveWithMeta(const std::string& name, const std::string& meta) {
    CajetaArchive a(name, "1.0", CajetaArchive::Kind::Cja);
    if (!meta.empty()) a.setNativeLibrariesMeta(bytesOf(meta));
    return a;
}
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
} // namespace

// 4.1.1 — union across two deps each declaring a distinct native req.
TEST(NativeResolverCollectTests, unionsTwoDeps) {
    auto a = archiveWithMeta("dep.a", R"({
        "requires":["zstd"],
        "libraries":{"zstd":{"version":"1.5.*","license":"BSD-3-Clause",
                             "redistributable":true}}})");
    auto b = archiveWithMeta("dep.b", R"({
        "requires":["lz4"],
        "libraries":{"lz4":{"version":"1.9.*","license":"BSD-2-Clause",
                            "redistributable":true}}})");
    std::vector<const CajetaArchive*> arcs{&a, &b};
    auto set = collectNativeRequirements(arcs);
    ASSERT_TRUE((bool) set) << errText(set.takeError());
    EXPECT_EQ(set->required.size(), 2u);
    EXPECT_EQ(set->libraries.size(), 2u);
    EXPECT_TRUE(set->libraries.count("zstd"));
    EXPECT_TRUE(set->libraries.count("lz4"));
    EXPECT_TRUE(set->unsatisfied.empty());
}

// 4.1.2 — same lib from two deps: deduped, both version constraints preserved.

// 4.1.3 — a required lib with no resolution metadata anywhere is unsatisfied.

// 4.3.1 — collection is order-independent (provider in a different dep).

// Slim archives (no native metadata) contribute nothing, no error.

// Malformed embedded metadata fails loud.
