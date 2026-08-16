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
TEST(SpecAcceptanceTests, nullIsRealAbsenceNotNaN) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.column.Column;\n"
        "import cajeta.nucleo.column.NullableColumn;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        // [1, 2, <missing>] — the third element is a true absence.
        "        float64[] nv = heap float64[3];\n"
        "        nv[0] = 1.0; nv[1] = 2.0; nv[2] = 0.0;\n"
        "        boolean[] ok = heap boolean[3];\n"
        "        ok[0] = true; ok[1] = true; ok[2] = false;\n"
        "        NullableColumn<float64> nc #= NullableColumn.of<float64>(nv, ok);\n"
        "        if (nc.mean() == 1.5) { score = score + 1; }\n"    // null skipped: (1+2)/2
        // [1, 2, NaN] — the third element is a NaN VALUE, not a missing.
        "        float64 z = 0.0;\n"
        "        float64 nan = z / z;\n"
        "        float64[] pv = heap float64[3];\n"
        "        pv[0] = 1.0; pv[1] = 2.0; pv[2] = nan;\n"
        "        Column<float64> pc #= Column.of<float64>(pv);\n"
        "        float64 m = pc.mean();\n"
        "        if (m != m) { score = score + 2; }\n"              // NaN propagated
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// §11 / §4.1 — a relational op yields a NEW table and leaves its input unchanged:
// no `inplace=`, no copy/view ambiguity. Filtering `t` produces a smaller result
// while `t` itself still holds every row.
TEST(SpecAcceptanceTests, relationalOpLeavesInputUnchanged) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.frame.Pred;\n"
        "import cajeta.nucleo.column.Column;\n"
        "public record Px { float64 v; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] vv = heap float64[4];\n"
        "        vv[0] = 1.0; vv[1] = 5.0; vv[2] = 2.0; vv[3] = 9.0;\n"
        "        Table<Px> t = heap Table<Px>(Column.of<float64>(vv));\n"
        "        int32 score = 0;\n"
        "        Table<Px> f #= t.lazy()\n"
        "            .filter((PxCols c) -> { Pred p = c.v() > 3.0; return #p; })\n"
        "            .collect();\n"
        "        if (f.rowCount() == 2) { score = score + 1; }\n"   // 5.0 and 9.0 survive
        "        if (t.rowCount() == 4) { score = score + 2; }\n"   // the input is untouched
        // re-deriving from the SAME t again yields the full set — no mutation stuck.
        "        Table<Px> g #= t.lazy().collect();\n"
        "        if (g.rowCount() == 4) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

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
