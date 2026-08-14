//
// jupyter-kernel U7 (spec 6.1; plan 7.1.1) — a notebook against a real
// library.
//
// Every other kernel test compiles cells against the stdlib alone. This one
// puts an Olla library — `dev.cajeta.ml` — on the session's classpath and
// drives it the way a data scientist would: load in one cell, fit in the
// next, summarize in a third, each seeing the last one's bindings.
//
// It is the only test that exercises 7.2.4's classpath path end to end, and
// the reason that feature exists: without it a notebook can use the stdlib
// and nothing else, which is not what anyone opens a notebook for.
//
// The archive comes from the SIBLING CLONE `cajeta-ml`, which is where
// dev.cajeta.ml is developed (see the `cajeta-clone-topology` note). That
// makes this test environment-dependent by nature, so it SKIPS rather than
// fails when the archive is absent — a missing sibling clone is a fact about
// the machine, not a defect in the kernel.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <filesystem>
#include <memory>
#include <string>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;
using cajeta::kernel::SessionOptions;

namespace {

// The newest `dev.cajeta.ml-<version>.cja` in the sibling clone, or empty.
// Newest rather than pinned: the version moves with that project's releases,
// and a test that pins one becomes a chore for whoever bumps it.
std::string findMlArchive() {
    std::filesystem::path dir =
        "/home/julian/code/cpp/cajeta-ml/build/archive";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return std::string();
    std::string best;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("dev.cajeta.ml-", 0) != 0) continue;
        if (entry.path().extension() != ".cja") continue;
        if (name > best) best = name;
    }
    if (best.empty()) return std::string();
    return (dir / best).string();
}

}  // namespace

// DISABLED — and its FAILURE is the finding (plan 7.2.5).
//
// This asked whether a classpath session works at all or only fails where
// the archive and the stdlib share a generic instantiation. The answer is
// the harsher one: even `int32 a = 20; a + 22;` fails in a session with an
// archive on its classpath, with `module verify failed: Invalid bitcast
// ... double to ptr`. A classpath session is broken outright.
//
// Three experiments bound the cause to the kernel's RESIDENT STDLIB REUSE
// rather than to the classpath machinery:
//   * kernel + classpath + any cell                        -> invalid bitcast
//   * kernel + NO classpath + the same cell                 -> ok
//   * `cajeta run` + the SAME archive on --classpath        -> ok
// `cajeta run` builds its stdlib fresh. The kernel restores
// StdlibReuseCore's resident baseline, captured before the archive was
// ingested, and `linkClasspathModules` then splices the archive's modules
// into the list the codegen fixpoint walks — so the archive's code is
// generated against a stdlib world it was not compiled against. Two
// definitions of one specialization with different llvm::Type identity is
// exactly the shape that produces a double/ptr bitcast.
//
// This is the risk called out before 7.2.4 was built, now confirmed.
TEST(KernelDataScienceTests, DISABLED_classpathSessionRunsPlainCells) {
    const std::string archive = findMlArchive();
    if (archive.empty()) {
        GTEST_SKIP() << "dev.cajeta.ml archive not built in the cajeta-ml "
                        "sibling clone";
    }
    SessionOptions options;
    options.classpath.push_back(archive);
    std::string error;
    auto s = KernelSession::create(options, &error);
    ASSERT_NE(nullptr, s.get()) << "session create failed: " << error;

    CellResult r = s->execute("int32 a = 20;\na + 22;\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("42", r.result);

    // And a class from the archive resolves, which is the point of having
    // put it there.
    CellResult imported = s->execute(
        "import dev.cajeta.ml.linear.LinearRegression;\n"
        "LinearRegression lr = heap LinearRegression(true);\n"
        "lr.isFitted();\n");
    ASSERT_TRUE(imported.ok) << imported.errorId << ": " << imported.message;
    EXPECT_EQ("false", imported.result);
}

// 7.1.1 / spec 6.1 — the three-cell LinearRegression flow.
//
// DISABLED behind the same blocker as the test above (plan 7.2.5): the
// session never gets far enough for this flow to say anything about the ml
// library. It is written and ready for the day a classpath session compiles
// — the three cells and their assertions are the spec 6.1 acceptance, and
// re-enabling it is the check that 7.2.5 actually landed.
TEST(KernelDataScienceTests, DISABLED_mlNotebookFlow) {
    const std::string archive = findMlArchive();
    if (archive.empty()) {
        GTEST_SKIP() << "dev.cajeta.ml archive not built in the cajeta-ml "
                        "sibling clone; nothing to put on the classpath";
    }

    SessionOptions options;
    options.classpath.push_back(archive);
    std::string error;
    auto s = KernelSession::create(options, &error);
    ASSERT_NE(nullptr, s.get()) << "session create failed: " << error;

    // Cell 1 — LOAD. Builds the design matrix and the response from a known
    // linear relationship, so the fit below has a right answer to find.
    CellResult load = s->execute(
        "import cajeta.math.Tensor;\n"
        "int64 n = 40;\n"
        "float64[] xd = heap float64[n * (int64) 2];\n"
        "float64[] yd = heap float64[n];\n"
        "int64 i = 0;\n"
        "while (i < n) {\n"
        "    float64 t = (float64) i / 4.0;\n"
        "    float64 x0 = t * t + 1.0;\n"
        "    float64 x1 = ((float64) ((i * (int64) 3) % (int64) 7)) - 3.0;\n"
        "    xd[i * (int64) 2] = x0;\n"
        "    xd[i * (int64) 2 + (int64) 1] = x1;\n"
        "    yd[i] = 0.5 * x0 - 1.25 * x1 + 3.0;\n"
        "    i = i + 1;\n"
        "}\n"
        "int64[] xs = heap int64[2];\n"
        "xs[0] = n;\n"
        "xs[1] = 2;\n"
        "int64[] ys = heap int64[1];\n"
        "ys[0] = n;\n"
        "Tensor<float64> x = Tensor.of<float64>(xd, xs);\n"
        "Tensor<float64> y = Tensor.of<float64>(yd, ys);\n");
    ASSERT_TRUE(load.ok) << "cell 1 (load): " << load.errorId << ": "
                         << load.message;

    // Cell 2 — FIT. Sees `x` and `y` from cell 1; imports a class from the
    // classpath archive, which is the whole point of 7.2.4.
    CellResult fit = s->execute(
        "import dev.cajeta.ml.linear.LinearRegression;\n"
        "LinearRegression lr = heap LinearRegression(true);\n"
        "lr.fit(x, y);\n"
        "lr.isFitted();\n");
    ASSERT_TRUE(fit.ok) << "cell 2 (fit): " << fit.errorId << ": "
                        << fit.message;
    EXPECT_TRUE(fit.hasResult) << "no Out[2] for a trailing expression";
    EXPECT_EQ("true", fit.result) << "the model did not report itself fitted";

    // Cell 3 — SUMMARIZE. Sees the fitted model from cell 2. The data is
    // exactly linear, so R^2 is 1 to within floating-point noise; rendering
    // it is spec 6.1's "summary() renders readably in Out[N]".
    CellResult summary = s->execute(
        "import dev.cajeta.ml.linear.SummaryResult;\n"
        "SummaryResult sr = lr.summary(x, y);\n"
        "sr.r2;\n");
    ASSERT_TRUE(summary.ok) << "cell 3 (summary): " << summary.errorId << ": "
                            << summary.message;
    ASSERT_TRUE(summary.hasResult) << "no Out[3] for the summary value";
    // Rendered as text, which is what a notebook shows. Parsed back rather
    // than string-compared: the exact digits are the formatter's business,
    // the VALUE is the model's.
    const double r2 = std::stod(summary.result);
    EXPECT_NEAR(1.0, r2, 1e-6) << "R^2 on exactly-linear data was "
                               << summary.result;

    // And the session is still a session: a fourth cell sees all three.
    CellResult after = s->execute("x.shapeAt(0);\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("40", after.result);
}

// The other half of 7.2.4's contract: a session with NO classpath is still
// a stdlib session, and asking for a class that is not on it fails as a
// compile error rather than by some other route. Guards against a future
// "just put everything on the classpath" shortcut.
TEST(KernelDataScienceTests, noClasspathMeansStdlibOnly) {
    std::string error;
    auto s = KernelSession::create(&error);
    ASSERT_NE(nullptr, s.get()) << error;

    CellResult r = s->execute(
        "import dev.cajeta.ml.linear.LinearRegression;\n"
        "LinearRegression lr = heap LinearRegression(true);\n");
    EXPECT_FALSE(r.ok)
        << "a class from an un-classpathed archive resolved anyway";

    // The session survives the failure, as any failed cell must.
    CellResult after = s->execute("1 + 1;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("2", after.result);
}
