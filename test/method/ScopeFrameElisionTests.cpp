//
// SIMD plan Phase 0.6: the per-method structured-concurrency scope frame
// (__cajeta_scope_save_top + __cajeta_scope_enter[heap-alloc] + scope_exit_to)
// is only needed when a method's body actually `spawn`s / `detach`es / has a
// `scope { }`. A spawn-free body owns no child tasks and must NOT emit it — the
// frame was the per-stripe overhead that made the xxhash3 SIMD loop slower than
// native. These golden-IR tests pin the elision (spawn-free -> no scope_enter
// call) and its safety (spawning / scope-block bodies keep the frame). The
// behavioral spawn/join semantics are covered by Spawn*/Async* tests.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string compileIr(const std::string& src) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    return jit->getModuleIr();
}

// True iff the `test.D::run` FUNCTION body contains a call to
// __cajeta_scope_enter. Scoped to run() because the module also links stdlib
// methods (e.g. cajeta.reflect.* throw on access denial → conservatively keep
// the frame) whose scope_enter is unrelated to the method under test.
bool callsScopeEnter(const std::string& ir) {
    size_t def = ir.find("define");
    while (def != std::string::npos) {
        size_t nl = ir.find('\n', def);
        std::string header = ir.substr(def, nl - def);
        if (header.find("test.D::run") != std::string::npos) {
            // Function body runs to the next top-level `\n}`.
            size_t end = ir.find("\n}", nl);
            std::string body = ir.substr(nl, (end == std::string::npos ? ir.size() : end) - nl);
            return body.find("call") != std::string::npos
                && body.find("@__cajeta_scope_enter") != std::string::npos;
        }
        def = ir.find("define", nl);
    }
    return false; // no run() defined -> vacuously no scope_enter
}

} // namespace

// A spawn-free method must elide the per-method scope frame.
TEST(ScopeFrameElisionTests, spawnFreeMethodElidesScopeEnter) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 s = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 100) { s = s + i; i = i + 1; }\n"
        "        return s;\n"
        "    }\n"
        "}\n";
    std::string ir = compileIr(src);
    // Show the enclosing define(s) of any scope_enter call to debug the source.
    std::string ctx;
    size_t p = 0;
    while ((p = ir.find("@__cajeta_scope_enter", p)) != std::string::npos) {
        size_t defp = ir.rfind("\ndefine", p);
        size_t nl = ir.find('\n', defp + 1);
        if (defp != std::string::npos)
            ctx += ir.substr(defp + 1, (nl == std::string::npos ? defp + 80 : nl) - defp - 1) + "\n";
        p += 1;
    }
    EXPECT_FALSE(callsScopeEnter(ir))
        << "spawn-free method should not call __cajeta_scope_enter; "
           "scope_enter appears in:\n" << ctx;
}

// (A top-level `await spawn` async-lowers run() into a continuation, moving the
// frame out of the `test.D::run` define — so it isn't cleanly IR-testable here.
// The "spawning method keeps its frame" property is covered precisely by the
// nested-spawn container tests below (function-scoped on run()) and behaviorally
// by AsyncSyntaxTests.{awaitSpawnReturnsInnerValue,implicitFunctionBodyScopeJoinsSpawn}.)

// ---- A spawn nested in any control-flow container must keep the frame. The
// walk must reach it through the statement's typed sub-node fields (which are
// NOT in getChildren()). One test per container so a gap fails loudly here
// rather than as an orphaned-task hang in the concurrency suite. ----
namespace {
std::string spawnInBody(const std::string& body) {
    return std::string(
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run(int32 n) {\n"
        "        int32 r = 0;\n"
        "        ") + body + "\n"
        "        return r;\n"
        "    }\n"
        "}\n";
}
void expectKeepsFrame(const std::string& body) {
    EXPECT_TRUE(callsScopeEnter(compileIr(spawnInBody(body))))
        << "spawn nested here must keep the scope frame; body:\n" << body;
}
}
TEST(ScopeFrameElisionTests, spawnInWhileKeepsFrame) {
    expectKeepsFrame("while (n > 0) { r = await spawn compute(); n = n - 1; }");
}
TEST(ScopeFrameElisionTests, spawnInForKeepsFrame) {
    expectKeepsFrame("for (int32 i = 0; i < n; i = i + 1) { r = await spawn compute(); }");
}
TEST(ScopeFrameElisionTests, spawnInIfKeepsFrame) {
    expectKeepsFrame("if (n > 0) { r = await spawn compute(); }");
}
TEST(ScopeFrameElisionTests, spawnInDoWhileKeepsFrame) {
    expectKeepsFrame("do { r = await spawn compute(); n = n - 1; } while (n > 0);");
}
TEST(ScopeFrameElisionTests, spawnInTryKeepsFrame) {
    expectKeepsFrame("try { r = await spawn compute(); } catch (Exception e) { r = 1; }");
}
TEST(ScopeFrameElisionTests, spawnInNestedBlockKeepsFrame) {
    expectKeepsFrame("{ { if (n > 0) { while (n > 0) { r = await spawn compute(); n = n - 1; } } } }");
}
// (Lambda isolation — a spawn only inside a nested lambda leaves the outer
// frame elided — is correct by construction: astHasSpawnSite returns false at a
// LambdaExpression. Getting it wrong only over-keeps the frame, which is safe,
// so it's not pinned by a test here.)

// A method with an explicit `scope { }` keeps the frame.
TEST(ScopeFrameElisionTests, scopeBlockMethodKeepsScopeEnter) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 5; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        scope { r = await spawn compute(); }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(callsScopeEnter(compileIr(src)))
        << "a method with scope { } must keep __cajeta_scope_enter";
}

// ---- tensor-element-access Unit 3: a throw, try or switch is not a spawn site.
// A method whose only trigger was one of them malloc'd a scope frame per call
// (Storage.get: 5 ns of a 15 ns read). Elision must not lose a spawn nested in
// a catch, finally or switch arm, and a throw must still join spawned children.
namespace {
std::string bodyWithImports(const std::string& body) {
    return std::string(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run(int32 n) {\n"
        "        int32 r = 0;\n"
        "        ") + body + "\n"
        "        return r;\n"
        "    }\n"
        "}\n";
}
void expectElides(const std::string& body) {
    EXPECT_FALSE(callsScopeEnter(compileIr(bodyWithImports(body))))
        << "no spawn here, so no scope frame; body:\n" << body;
}
void expectKeeps(const std::string& body) {
    EXPECT_TRUE(callsScopeEnter(compileIr(bodyWithImports(body))))
        << "spawn nested here must keep the scope frame; body:\n" << body;
}
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -100;
    return fn();
}
}
TEST(ScopeFrameElisionTests, throwOnlyMethodElidesScopeEnter) {
    expectElides("if (n < 0) { throw heap Exception(\"negative\"); }\n        r = n;");
}
TEST(ScopeFrameElisionTests, tryWithoutSpawnElidesScopeEnter) {
    expectElides("try { r = n + 1; } catch (Exception e) { r = 2; } finally { r = r + 1; }");
}
TEST(ScopeFrameElisionTests, switchWithoutSpawnElidesScopeEnter) {
    expectElides("switch (n) { case 0: r = 1; break; default: r = 2; break; }");
}
TEST(ScopeFrameElisionTests, spawnInCatchKeepsFrame) {
    expectKeeps("try { r = n; } catch (Exception e) { r = await spawn compute(); }");
}
TEST(ScopeFrameElisionTests, spawnInFinallyKeepsFrame) {
    expectKeeps("try { r = n; } finally { r = await spawn compute(); }");
}
TEST(ScopeFrameElisionTests, spawnInSwitchArmKeepsFrame) {
    expectKeeps("switch (n) { case 0: r = await spawn compute(); break; default: r = 2; break; }");
}
TEST(ScopeFrameElisionTests, spawnAndThrowKeepsFrame) {
    expectKeeps("if (n < 0) { throw heap Exception(\"negative\"); }\n        r = await spawn compute();");
}

// 2.2: a frameless throw reaches the caller with its type, message and frames.
TEST(ScopeFrameElisionTests, framelessThrowKeepsTypeMessageAndFrames) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static int32 boom(int32 n) {\n"
        "        if (n < 0) { throw heap Exception(\"negative input\"); }\n"
        "        return n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            D.boom(-1);\n"
        "            return -1;\n"
        "        } catch (Exception e) {\n"
        "            if (!e.getMessage().equals(\"negative input\")) { return -2; }\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            boolean sawBoom = false;\n"
        "            boolean sawRun = false;\n"
        "            int64 i = 0;\n"
        "            while (i < fs.count()) {\n"
        "                if (fs[i].method.equals(\"boom\")) { sawBoom = true; }\n"
        "                if (fs[i].method.equals(\"run\")) { sawRun = true; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (!sawBoom) { return -3; }\n"
        "            if (!sawRun) { return -4; }\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n"), 1);
}

// 2.3, 2.4: a method that spawns and then throws, directly or through a frameless
// callee, has its child joined before the catch body runs.
TEST(ScopeFrameElisionTests, throwAfterSpawnJoinsTheChild) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 done = 0;\n"
        "    public static async int32 work() {\n"
        "        int64 s = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < 3000000) { s = s + i; i = i + 1; }\n"
        "        D.done = D.done + 1;\n"
        "        return (int32) (s & 1);\n"
        "    }\n"
        "    public static void boom() { throw heap Exception(\"callee\"); }\n"
        "    public static void direct() {\n"
        "        Task<int32> t = spawn work();\n"
        "        throw heap Exception(\"direct\");\n"
        "    }\n"
        "    public static void viaCallee() {\n"
        "        Task<int32> t = spawn work();\n"
        "        D.boom();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        try { D.direct(); return -1; } catch (Exception e) { if (D.done != 1) { return -2; } }\n"
        "        try { D.viaCallee(); return -3; } catch (Exception e) { if (D.done != 2) { return -4; } }\n"
        "        return 1;\n"
        "    }\n"
        "}\n"), 1);
}
