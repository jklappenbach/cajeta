// lazy-codegen Unit 5 (spec 2.4, 2.2.1, 2.2.2, 4.2) — the reflective surface
// under demand-driven emission, in a real kernel session.
//
// The machinery landed in Units 3-4 (thunks indexed and generated on demand,
// registration gated by the per-cell keep-set, late keeps delivered in a
// later cell's init delta); these tests pin the behaviour the spec promises.

#include "gtest/gtest.h"

#include "cajeta/jit/LazyCodegen.h"
#include "cajeta/kernel/KernelSession.h"

#include <memory>
#include <string>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;
using cajeta::lazyCodegenEnabled;
using cajeta::setLazyCodegenEnabled;

namespace {

struct ModeGuard {
    bool saved = lazyCodegenEnabled();
    ~ModeGuard() { setLazyCodegenEnabled(saved); }
};

std::unique_ptr<KernelSession> lazySession() {
    setLazyCodegenEnabled(true);
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_NE(nullptr, s.get()) << "session create failed: " << error;
    return s;
}

}  // namespace

// 5.1.1 + 5.1.4 — reflection invokes a method through a keep-set class's
// method table. The target is a STDLIB class: a kernel cell class carries a
// reduced RTTI method table in BOTH modes (measured: one entry, eager and
// lazy alike — pre-existing shape, not a lazy defect), so the stdlib is
// where "the thunk and its target are generated on demand" is observable.
// Under lazy neither String's reflect_invoke thunk nor count()'s body was
// eagerly emitted; both must arrive through the generator for 5.
TEST(LazyReflectionTests, reflectiveInvokeThroughAKeepSetClass) {
    ModeGuard guard;
    auto s = lazySession();
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        "import cajeta.reflect.Class;\n"
        "import cajeta.reflect.Method;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "int32 probe() {\n"
        "    Optional<Class<?>> co #= Class.forName(\"cajeta.lang.String\");\n"
        "    if (!co.isPresent()) { return -1; }\n"
        "    Class<?> c = co.get();\n"
        "    String target = \"hello\";\n"
        "    int32 n = c.getMethodCount();\n"
        "    int32 out = 1000 + n;\n"
        "    for (int32 i = 0; i < n; i++) {\n"
        "        String nm #= c.getMethodName(i);\n"
        "        if (nm.contains(\"::count(\")) {\n"
        "            Method m #= c.getMethod(i);\n"
        "            out = (int32) m.invokeScalar(target);\n"
        "        }\n"
        "    }\n"
        "    return out;\n"
        "}\n"
        "probe();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("5", r.result);
    s->shutdown();
}

// 5.1.2 — Class.heapInstance<Shape>(name) constructs a subtype whose thunks
// were never emitted: the <Shape> bound is the keep-set root (BoundClosure,
// spec 2.2.1), so the whole family registers in this cell's delta, and the
// subtype's reflect_new thunk arrives through the generator. The name comes
// from Class.of(...).getName() so the test never guesses the canonical form.
TEST(LazyReflectionTests, boundedHeapInstanceConstructsAnUnemittedSubtype) {
    ModeGuard guard;
    auto s = lazySession();
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "public class Shape {\n"
        "    public int32 sides;\n"
        "    public Shape() { this.sides = 0; return; }\n"
        "    public int32 sideCount() { return this.sides; }\n"
        "}\n"
        "public class Circle extends Shape {\n"
        "    public Circle() { this.sides = 7; return; }\n"
        "}\n"
        "int32 probe() {\n"
        "    Circle known = heap Circle();\n"
        "    String nm #= Class.of(known).getName();\n"
        "    Optional<Shape> made #= Class.heapInstance<Shape>(nm);\n"
        "    int32 out = 0;\n"
        "    if (made.isPresent()) {\n"
        "        Shape sh = made.get();\n"
        "        out = sh.sideCount();\n"
        "    }\n"
        "    return out;\n"
        "}\n"
        "probe();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("7", r.result);
    s->shutdown();
}

// 5.1.3 — a non-literal forName is an OPEN site: it forces keep-all for that
// cell, exactly as it does under AOT DCE (spec 2.2.2 — the two mechanisms
// answer "what must survive" identically). The class it names was defined a
// cell EARLIER with no reflection anywhere near it, so only the forced keep
// can have registered it. Also pins the 5.1.3 fix: snapshots must legalize
// foreign globals reachable through METADATA, or the bitcode writer's
// unchecked ValueID switch computes a wild jump on the first runtime-function
// snapshot whose shared-context debug metadata names another module's copy.
TEST(LazyReflectionTests, dynamicForNameForcesKeepAllAcrossCells) {
    ModeGuard guard;
    auto s = lazySession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Ghost { public Ghost() { return; }\n"
        "  public int32 boo() { return 13; } }\n"
        "Ghost g = heap Ghost();\n"
        "g.boo();\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_EQ("13", c1.result);

    CellResult c2 = s->execute(
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "int32 probe() {\n"
        "    Ghost g = heap Ghost();\n"
        "    String full #= Class.of(g).getName();\n"
        "    Optional<Class<?>> found #= Class.forName(full);\n"
        "    if (found.isPresent()) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "probe();\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    ASSERT_TRUE(c2.hasResult);
    EXPECT_EQ("1", c2.result);
    s->shutdown();
}

// The staging half of 2.2.1/2.2.2 that only a kernel has: a stdlib class no
// earlier cell kept is registered LATE — a later cell's literal forName
// keeps it, its registration ctor is born in that cell's compile, and the
// cell's init DELTA delivers it (4.2.4's late keep growth). The target is a
// non-template class: an uninstantiated template has no registrable
// #ClassObject, so `forName("...ArrayList")` is empty in ANY mode.
TEST(LazyReflectionTests, literalForNameInALaterCellRegistersLate) {
    ModeGuard guard;
    auto s = lazySession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("20 + 42;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c2 = s->execute(
        "import cajeta.reflect.Class;\n"
        "import cajeta.lang.Optional;\n"
        "int32 probe() {\n"
        "    Optional<Class<?>> c #= Class.forName(\"cajeta.lang.String\");\n"
        "    if (c.isPresent()) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "probe();\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    ASSERT_TRUE(c2.hasResult);
    EXPECT_EQ("1", c2.result);
    s->shutdown();
}
