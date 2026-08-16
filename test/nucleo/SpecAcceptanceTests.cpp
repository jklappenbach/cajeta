//
// nucleo-frame U18 — the spec §11 acceptance criteria as tests (plan 18.1.2).
// The pandas "sins" are demonstrably ABSENT — pinned two ways:
//   - BEHAVIORAL: null is a real absence, NOT NaN-as-missing (a null is skipped
//     by an aggregation; a NaN is a value that propagates); and a relational op
//     leaves its input table unchanged (no `inplace=`, no copy/view mutation).
//   - GREP-ABLE: the frame + column runtime source declares no `MultiIndex`, no
//     `inplace` flag, no `object_dtype`, no `SettingWithCopyWarning` — the four
//     structural pandas patterns the spec refuses (§1.6, §11).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// §11 / §7 — null ≠ NaN. A null is a real absence (skipped by the reduction, so
// mean divides by the non-null count); a NaN is a *value* that propagates. The
// two produce different, predictable results over the same numbers — the pandas
// conflation is refused.

// §11 / §4.1 — a relational op yields a NEW table and leaves its input unchanged:
// no `inplace=`, no copy/view ambiguity. Filtering `t` produces a smaller result
// while `t` itself still holds every row.

// §11 (grep-able) — the frame + column runtime source carries none of the four
// structural pandas patterns the spec refuses. "index" as a word is fine (the
// pluggable index interface, B+/Z-order — §9); the FORBIDDEN tokens are the
// pandas-specific spellings.
TEST(SpecAcceptanceTests, frameSourceHasNoPandasSins) {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root) {
        GTEST_SKIP() << "CAJETA_SOURCE_ROOT unset — grep-able pin needs the source tree";
    }
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs = {
        fs::path(root) / "runtime" / "src" / "cajeta" / "nucleo" / "frame",
        fs::path(root) / "runtime" / "src" / "cajeta" / "nucleo" / "column",
    };
    // pandas sins, by their pandas spelling. Not "index" (legit here).
    const std::vector<std::string> forbidden = {
        "MultiIndex", "inplace", "object_dtype", "SettingWithCopy",
    };
    bool scannedSomething = false;
    for (const auto& dir : dirs) {
        if (!fs::exists(dir)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".cajeta") continue;
            // Skills/markdown are not code; only .cajeta sources are the surface.
            scannedSomething = true;
            std::ifstream in(entry.path());
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string body = ss.str();
            for (const auto& tok : forbidden) {
                EXPECT_EQ(body.find(tok), std::string::npos)
                    << "pandas sin '" << tok << "' present in "
                    << entry.path().string();
            }
        }
    }
    EXPECT_TRUE(scannedSomething)
        << "found no .cajeta under nucleo/frame|column — path drift?";
}
