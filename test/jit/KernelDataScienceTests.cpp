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

// 7.2.5 — a classpath session compiles and runs plain cells.
//
// This was DISABLED for a long time behind a failure that got diagnosed wrong
// twice. Both wrong answers are recorded here because the failure mode is a
// convincing liar and the next person to meet it deserves the warning.
//
// What it actually was: `fresh` — the set of modules a cell delivers to ORC —
// could contain the SESSION'S OWN STDLIB MODULE TWICE. `ensureStdlibModule`
// pushes the stdlib into the building compiler's module list and early-returns
// for every later compiler, so a RESIDENT session (which inherits a stdlib
// somebody else built) does not see it in `getModules()` while a CLASSPATH
// session (which builds its own, the reuse core being unavailable to it) does
// — and the delivery path pushed it again unconditionally on top. `addIRModule`
// was called twice with the same bitcode.
//
// The two wrong answers, in order:
//
//   1. "The resident baseline was captured before the archive existed, so
//      archive code is generated against a stdlib world it was not compiled
//      against." Plausible, and it explained the `Invalid bitcast double ->
//      ptr` neatly. Building the stdlib FRESH for classpath sessions did not
//      fix anything. (The change was right for other reasons and stayed.)
//
//   2. "A `.cja` is self-contained, so it brings its own copies of stdlib code
//      and they collide with the session's." The error certainly reads that
//      way: `duplicate definition of 'cajeta.math.Color::linearToSrgbChannel'`.
//      Demoting the archive's copy to a declaration appeared to advance the
//      failure to a new stdlib family each run — Color, then
//      cajeta.nucleo.frame.Exec, then cajeta.reflect.Constructor, then a
//      synthesized `reflect_invoke` thunk — which read as chasing a set nearly
//      the size of the library, and the conclusion drawn was that no
//      incremental fix converges. Wrong, twice over: the archive holds 144
//      `dev.cajeta.ml.*` modules and not one `cajeta.*` module, and the
//      "different family" each run was ORC naming whichever symbol its
//      hash-ordered table reached first in the SAME duplicated module. The
//      demotion pass never demoted a single symbol.
//
// The lesson worth keeping: a duplicate-definition error names an arbitrary
// symbol, so the symbol it names is evidence about nothing. Ask which MODULE
// is being added twice before reading anything into which name it mentions.
//
// One more defect fell out downstream, in the compiler rather than the kernel:
// `dev.cajeta.ml.grad.GradTape`'s WILDCARD instantiation carried an invalid
// `bitcast` between `ptr` and `double` (a `T`-typed value cast to `float64`),
// so it failed to verify and took the ~40 modules referencing it down with it.
// See WildcardScalarCastTests.
TEST(KernelDataScienceTests, classpathSessionRunsPlainCells) {
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

// 7.1.1 / spec 6.1 — the three-cell LinearRegression flow: load, fit,
// summarize, each cell seeing the last one's bindings. This is the spec 6.1
// acceptance and the reason the classpath feature exists — a notebook that can
// use the stdlib and nothing else is not what anyone opens a notebook for.
TEST(KernelDataScienceTests, mlNotebookFlow) {
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
