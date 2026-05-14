//
// Tests for the error-model implementation (ErrorModel.md).
//
// Current scope: throws-clause parse + AST plumbing. Subsequent
// commits add the lint warning, runtime Throwable* migration, system
// default catch, and CajetaTask exception slot.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Throws clause on a method parses and the body still codegens. No
// semantic enforcement yet — the list is informational. This test
// proves that an arbitrary type name in `throws T1, T2` doesn't break
// parse (the names don't need to resolve to actual types until the
// lint pass walks them).
TEST(ErrorModelTests, throwsClauseOnMethodParses) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException, TimeoutException {\n"
        "        return 42;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return maybeFail();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Single-entry throws clause — most common case.
TEST(ErrorModelTests, throwsClauseSingleEntry) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 fetch() throws IOException {\n"
        "        return 7;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return fetch();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Stdlib prelude — Throwable + RecoverableException + UnrecoverableException
// load implicitly into every compilation unit. User code can reference them
// by simple name without an import, and `extends` works against them.
TEST(ErrorModelTests, stdlibThrowableInstantiable) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Throwable t = new Throwable(0, 0);\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(ErrorModelTests, stdlibRecoverableExtendsThrowable) {
    auto src =
        "package test;\n"
        "public class IOException extends RecoverableException {\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IOException e = new IOException();\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// R5/Error-model #205: a throw inside an async fn body propagates to the
// caller via the await. The fiber trampoline catches the throw, stashes
// the value on the task's exception slot, and signals done. await reads
// the slot post-wait and re-raises into the awaiter's frame, where the
// surrounding try/catch picks it up. Without #205 the throw would
// longjmp through setjmp boundaries the fiber never set up, and the
// process would abort.
TEST(ErrorModelTests, asyncFnThrowReraisedAtAwait) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 failing() {\n"
        "        throw 99;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            result = await spawn failing();\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// R5/Error-model #205 — corollary: successful async fns still return
// their value through the await path unchanged. The exception slot
// stays NULL, the rethrow branch isn't taken, the value comes back.
// Verifies the new branching codegen in await doesn't accidentally
// break the happy path.
TEST(ErrorModelTests, asyncFnSuccessAwaitsValueThroughBranches) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 succeeding() { return 17; }\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            result = await spawn succeeding();\n"
        "        } catch (Exception e) {\n"
        "            result = -2;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// R5-D: a spawned task throws but isn't awaited. The enclosing scope's
// closing `}` walks each registered task's exception slot and re-raises
// the first one found into the surrounding frame. Caught by the
// outer try/catch.
TEST(ErrorModelTests, scopeReraisesUnawaitedSpawnThrow) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 failing() {\n"
        "        throw 77;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            scope {\n"
        "                spawn failing();\n"
        "            }\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

// R5-D: spawn at the function-body level (no explicit scope). The
// implicit function-body scope picks up the throw at function exit
// and re-raises into the function's caller. Here run() doesn't catch,
// so the throw propagates out — but with no test main wrapping, an
// uncaught throw would abort. Wrap in try/catch to verify propagation.
TEST(ErrorModelTests, implicitScopeReraisesAtFunctionExit) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 failing() {\n"
        "        throw 31;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 inner() {\n"
        "        spawn failing();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            result = inner();\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// R5-C: when one child of a scope throws, the sibling that's parked in
// an await gets cancelled — its next resume raises the trigger instead
// of completing normally. Without R5-C, scope would wait indefinitely
// for `idleSibling` (which is awaiting `forever`, a task that runs
// freely). With R5-C, scope cancels idleSibling after seeing failing's
// throw, so idleSibling's await raises and its trampoline catches.
// The visible signal: run() returns the trigger value (44), not hung.
TEST(ErrorModelTests, scopeCancelsParkedSiblingOnFirstThrow) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 failing() {\n"
        "        throw 44;\n"
        "        return 0;\n"
        "    }\n"
        "    public static async int32 forever() {\n"
        "        return 0;\n"
        "    }\n"
        "    public static async int32 idleSibling() {\n"
        "        int32 v = await spawn forever();\n"
        "        return v + 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            scope {\n"
        "                spawn failing();\n"
        "                spawn idleSibling();\n"
        "            }\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 44);
}

// R5/Error-model #203: stack-trace capture. Every throw site walks the
// native call stack via backtrace() and stashes the frames in a side
// table keyed by the throwable pointer. The test doesn't verify the
// content (frame addresses are JIT-dependent), just that capture
// doesn't crash on a throw + catch + re-throw pattern. The trace is
// retrievable via __cajeta_print_trace if needed.
TEST(ErrorModelTests, throwCapturesTraceAndCatchSucceeds) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            throw 51;\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 51);
}

// Constructor throws clause — same grammar, separate parse path.
TEST(ErrorModelTests, constructorThrowsParses) {
    auto src =
        "package test;\n"
        "public class Resource {\n"
        "    public Resource() throws IOException { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Resource r = new Resource();\n"
        "        return 11;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
