// lazy-codegen Unit 4 (spec 2.1, 2.2, 2.3, 4.1, 4.5) — the kernel host stops
// eagerly emitting ordinary method bodies when lazy codegen is on.
//
// The parity tests are RAILS, not discovery: they pass against the eager
// fixpoint today and must stay green as it is cut down. The emission-count
// test is the behavioural pin — it fails while the kernel still emits the
// world eagerly under the lazy flag, and is what 4.2.1 turns green.

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

}  // namespace

// 4.1.1 — a cell's observable result is identical lazy vs eager, toggled
// in-process. One session per mode, the same cells through both.
TEST(LazyKernelTests, cellResultsAgreeLazyVsEager) {
    ModeGuard guard;

    // The stdlib instantiation is the FIRST cell on purpose: a pure-stdlib
    // specialization first used in a LATER cell lands in the live stdlib
    // module after that module was delivered, and the eager kernel never
    // redelivers it — "Symbols not found: ArrayList<int32>#VTable, ::get,
    // ::ArrayList, ::add" (pre-existing; found writing this test 2026-08-17).
    // Parity is only comparable where the eager path works; the late-first-use
    // shape is pinned lazily by lateFirstUseInstantiationResolvesLazily.
    const std::vector<std::string> cells = {
        // stdlib template instantiation (first use at first cell)
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "xs.add(35);\n"
        "xs.get(0);\n",
        // class + heap + virtual dispatch
        "public class Point { public int32 x; public int32 y;\n"
        "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
        "  public int32 sum() { return this.x + this.y; } }\n"
        "Point p = heap Point(4, 5);\n"
        "p.sum();\n",
        // arithmetic result
        "20 + 42;\n",
        // exception thrown and caught in-cell (link-order sensitive)
        "import cajeta.error.RecoverableException;\n"
        "int32 caught() {\n"
        "    try { throw heap RecoverableException(\"boom\"); }\n"
        "    catch (RecoverableException e) { return 21; }\n"
        "    return 0;\n"
        "}\n"
        "caught();\n",
    };

    setLazyCodegenEnabled(false);
    auto eager = session();
    ASSERT_NE(nullptr, eager.get());
    std::vector<CellResult> eagerResults;
    for (size_t i = 0; i < cells.size(); ++i) {
        eagerResults.push_back(eager->execute(cells[i]));
        ASSERT_TRUE(eagerResults.back().ok)
            << "cell " << i << " (eager): " << eagerResults.back().errorId
            << ": " << eagerResults.back().message;
    }
    eager->shutdown();
    eager.reset();

    setLazyCodegenEnabled(true);
    auto lazy = session();
    ASSERT_NE(nullptr, lazy.get());
    for (size_t i = 0; i < cells.size(); ++i) {
        CellResult r = lazy->execute(cells[i]);
        ASSERT_TRUE(r.ok) << "cell " << i << " (lazy): " << r.errorId << ": "
                          << r.message;
        EXPECT_EQ(eagerResults[i].hasResult, r.hasResult) << "cell " << i;
        EXPECT_EQ(eagerResults[i].result, r.result) << "cell " << i;
    }
    lazy->shutdown();
}

// The shape the eager kernel cannot serve (see the note in the parity test):
// a pure-stdlib specialization whose FIRST use is a later cell. The lazy
// generator resolves its symbols from the live world, so under lazy this
// sequence must work — it is a capability the default flip delivers, not
// just a cost win.
TEST(LazyKernelTests, lateFirstUseInstantiationResolvesLazily) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult c1 = s->execute("20 + 42;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c2 = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "xs.add(35);\n"
        "xs.get(0);\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    ASSERT_TRUE(c2.hasResult);
    EXPECT_EQ("35", c2.result);
    s->shutdown();
}

// 4.1.2 — dylib-init work stays eager (spec 2.2, 2.3): the class registry is
// populated (forName on a stdlib class hits — registration ctors ran) and a
// cell class's static initializer has run, under lazy.
TEST(LazyKernelTests, classRegistrationAndStaticInitializersRunLazily) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute(
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "public class Marker { public static int32 seed = 17;\n"
        "  public Marker() { return; } }\n"
        "int32 probe() {\n"
        "    Optional<Class<?>> c #= Class.forName(\"cajeta.lang.String\");\n"
        "    int32 found = 0;\n"
        "    if (c.isPresent()) { found = 100; }\n"
        "    return found + Marker.seed;\n"
        "}\n"
        "probe();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("117", r.result);
    s->shutdown();
}

// 4.1.3 — a redefinition in a later cell is served, not a stale lazily
// generated body from the earlier definition.
TEST(LazyKernelTests, redefinitionDoesNotServeAStaleBody) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult c1 = s->execute("int32 value() { return 1; }\nvalue();\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_EQ("1", c1.result);

    CellResult c2 = s->execute("int32 value() { return 2; }\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;

    CellResult c3 = s->execute("value();\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;
    EXPECT_EQ("2", c3.result);
    s->shutdown();
}

// 4.2.1 pin / 4.3.2 — under lazy the kernel does NOT run the world through
// generateCode: the eager count collapses from ~23k invocations to the cell's
// own handful, and at least one body arrives through the generator instead.
// (4.1.4 rides along: the mostly-declaration modules must verify clean.)
TEST(LazyKernelTests, lazyCellSkipsTheEagerWorld) {
    ModeGuard guard;
    setLazyCodegenEnabled(true);

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute("20 + 42;\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("62", r.result);

    const auto& st = s->stats();
    // The eager fixpoint made ~23,394 generateCode calls for this cell
    // before 4.2.1. The cell's own module plus its instantiation tail is
    // orders of magnitude smaller; 2,000 is a loose ceiling, not a target.
    EXPECT_LT(st.eagerBodiesGenerated, 2000)
        << "the kernel still emits the world eagerly under the lazy flag";
    s->shutdown();
}
