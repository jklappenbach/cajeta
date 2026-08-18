// lazy-codegen Unit 7 (spec 5.1, 5.1.1) — keep the eager path honest.
//
// `CAJETA_EAGER_CODEGEN=1` is a permanent supported control, which makes
// eager emission a second code path that must keep working. This suite is
// the exercise: a representative cross-section of session behaviour runs
// under BOTH emission modes via the in-process toggle (2.1.5) and asserts
// the same known results. One session per mode, many cells per session —
// the eager world is emitted once per session, so cell count is nearly
// free but session count is not (7.2.2).
//
// Deliberately NOT covered: the late-first-use stdlib instantiation shape,
// which the eager kernel cannot serve (pre-existing; pinned lazily by
// lateFirstUseInstantiationResolvesLazily) — the stdlib instantiation
// below is the FIRST cell for exactly that reason.

#include "gtest/gtest.h"

#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/kernel/KernelSession.h"

#include <memory>
#include <string>
#include <vector>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;
using cajeta::lazyCodegenEnabled;
using cajeta::setLazyCodegenEnabled;

namespace {

struct ModeGuard {
    bool saved = lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

std::unique_ptr<KernelSession> session() {
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_NE(nullptr, s.get()) << "session create failed: " << error;
    return s;
}

struct Cell {
    const char* label;
    const char* src;
    const char* want;
};

const Cell kCells[] = {
    {"stdlib-instantiation",
     "import cajeta.collection.ArrayList;\n"
     "ArrayList<int32> xs = heap ArrayList<int32>();\n"
     "xs.add(35);\n"
     "xs.get(0);\n",
     "35"},
    {"class-heap-virtual",
     "public class Point { public int32 x; public int32 y;\n"
     "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
     "  public int32 sum() { return this.x + this.y; } }\n"
     "Point p = heap Point(4, 5);\n"
     "p.sum();\n",
     "9"},
    {"arithmetic", "20 + 42;\n", "62"},
    {"string-concat-count",
     "import cajeta.lang.String;\n"
     "String s #= \"abc\" + \"de\";\n"
     "s.count();\n",
     "5"},
    {"exception-caught",
     "import cajeta.error.RecoverableException;\n"
     "int32 caught() {\n"
     "    try { throw heap RecoverableException(\"boom\"); }\n"
     "    catch (RecoverableException e) { return 21; }\n"
     "    return 0;\n"
     "}\n"
     "caught();\n",
     "21"},
    {"forName-literal",
     "import cajeta.reflect.Class;\n"
     "import cajeta.lang.Optional;\n"
     "int32 probe() {\n"
     "    Optional<Class<?>> found #= Class.forName(\"cajeta.lang.String\");\n"
     "    if (found.isPresent()) { return 1; }\n"
     "    return 0;\n"
     "}\n"
     "probe();\n",
     "1"},
};

}  // namespace

class EmissionModeParity : public ::testing::TestWithParam<bool> {};

// 7.1.1 — the cross-section, one mode per instantiation. Each cell asserts
// its KNOWN value, so a mode that drifts fails on its own rather than only
// relative to the other.
TEST_P(EmissionModeParity, representativeCellsHoldTheirValues) {
    ModeGuard guard;
    setLazyCodegenEnabled(GetParam());

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    for (const Cell& c : kCells) {
        CellResult r = s->execute(c.src);
        ASSERT_TRUE(r.ok) << c.label << ": " << r.errorId << ": " << r.message;
        ASSERT_TRUE(r.hasResult) << c.label;
        EXPECT_EQ(c.want, r.result) << c.label;
    }
    s->shutdown();
}

INSTANTIATE_TEST_SUITE_P(BothModes, EmissionModeParity,
                         ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("lazy")
                                               : std::string("eager");
                         });

// 7.1.2 — eager still emits its whole front-end world. NOT pinned to the
// plan's pre-change 12,379: that number tracked FRONT-END materialization,
// which narrowed independently (measured writing this test: the same
// trivial cell now walks ~3,656 methods over 2 deduped module entries,
// vs 23,394-with-duplicates in the 2026-08-16 reference — prime
// materializes ~30% of the stdlib today). The eager fixpoint itself is
// unfiltered (`if (lazyBodies && ...) continue;` is its only gate), so
// the honest pin is structural: thousands of bodies (a cell-module-only
// leak would be hundreds), zero routed through the generator, and the
// cross-section above green.
TEST(EmissionModeParity, eagerStillEmitsTheWholeWorld) {
    ModeGuard guard;
    setLazyCodegenEnabled(false);

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute("20 + 42;\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("62", r.result);

    const auto& st = s->stats();
    EXPECT_GT(st.eagerBodiesGenerated, 3000)
        << "the eager path no longer emits the whole front-end world";
    EXPECT_EQ(st.lazyBodiesDelivered, 0)
        << "eager mode routed bodies through the generator";
    s->shutdown();
}
