//
// Tests for the error-model implementation (ErrorModel.md).
//
// Current scope: throws-clause parse + AST plumbing. Subsequent
// commits add the lint warning, runtime Throwable* migration, system
// default catch, and CajetaTask exception slot.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
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
        "        Throwable t = heap Throwable(\"oops\");\n"
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
        "        IOException e = heap IOException();\n"
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

// #208: subclass constructor writes inherited field. The fix gave the
// subclass struct the same layout as the parent for inherited fields
// (parent fields prepended), so a GEP for an inherited field works
// on the subclass instance the same way it does on the parent. Before
// the fix, the subclass struct only carried its own fields after the
// vtable, so writing this.message (where message is defined on the
// parent) wrote past the end of the subclass's storage.
TEST(ErrorModelTests, subclassWritesInheritedField) {
    auto src =
        "package test;\n"
        "public class Parent {\n"
        "    public int32 marker;\n"
        "    public Parent() { this.marker = 0; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public Child(int32 value) {\n"
        "        this.marker = value;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Child c = heap Child(73);\n"
        "        int32 r = c.marker;\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 73);
}

// #208 follow-up: subclass own field still works after the layout
// change. Subclass struct now has [vtable, parent_fields, own_fields];
// own fields land at indices after the inherited ones, and
// getFieldLlvmIndex's countInheritedFields() + order + 1 formula
// produces the right slot.
TEST(ErrorModelTests, subclassWritesOwnFieldAfterInherited) {
    auto src =
        "package test;\n"
        "public class Parent {\n"
        "    public int32 a;\n"
        "    public Parent() { this.a = 0; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int32 b;\n"
        "    public Child(int32 a, int32 b) {\n"
        "        this.a = a;\n"
        "        this.b = b;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Child c = heap Child(40, 2);\n"
        "        int32 a = c.a;\n"
        "        int32 b = c.b;\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// #208 stress: three-level inheritance. Grandchild constructor writes a
// grandparent field; reads pull from every level. Exercises the recursive
// `countInheritedFields` (two levels deep) and `getFieldLlvmIndex`'s
// ancestor-walk in CajetaClass.h, plus DotExpression's recursive findProp
// in resolveTypes / generateCode. Layout for `Child` must be
// { vtable, g (i32), p (i32), c (i32) } so g lands at LLVM index 1, p at 2,
// c at 3 — independent of which level declared each.
TEST(ErrorModelTests, threeLevelInheritedFieldReadWrite) {
    auto src =
        "package test;\n"
        "public class Grandparent {\n"
        "    public int32 g;\n"
        "    public Grandparent() { this.g = 0; }\n"
        "}\n"
        "public class Parent extends Grandparent {\n"
        "    public int32 p;\n"
        "    public Parent() { this.p = 0; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int32 c;\n"
        "    public Child(int32 g, int32 p, int32 c) {\n"
        "        this.g = g;\n"
        "        this.p = p;\n"
        "        this.c = c;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Child obj = heap Child(100, 20, 3);\n"
        "        return obj.g + obj.p + obj.c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 123);
}

// #208 stress: mixed slot widths across inherited and own fields. The
// parent's int32 field must be stored at i32 width (not promoted to i64
// from a wide-default literal) so it doesn't trample the child's int64
// field that sits right after. Also exercises the `(int64) c.a` cast on
// an inherited field, which loads through the GEP to the i32 value and
// sign-extends — without that load, the cast would ptrtoint the GEP
// address and produce uninitialized-stack-shaped garbage.
TEST(ErrorModelTests, twoLevelMixedFieldWidths) {
    auto src =
        "package test;\n"
        "public class Parent {\n"
        "    public int32 a;\n"
        "    public Parent() { this.a = 0; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int64 q;\n"
        "    public Child() {\n"
        "        this.a = 7;\n"
        "        this.q = 12345;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Child c = heap Child();\n"
        "        return c.q + (int64) c.a;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 12352LL);
}

// int8 as a documented native type. Previously crashed during codegen
// because `INT8` / `UINT8` were referenced in the parser grammar but
// not defined in the lexer, so source `int8` lexed as IDENTIFIER and
// fell through to class-type lookup. canonicalMap missed and returned
// a null CajetaTypePtr that segfaulted when CajetaClass::generatePrototype
// called getLlvmType() on it for the struct layout. Fix: add INT8/UINT8
// lexer tokens and register `int8`/`uint8` in CajetaType::init.
TEST(ErrorModelTests, int8FieldOnClass) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public int8 b;\n"
        "    public Holder() { this.b = (int8) 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return (int32) h.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// int8 through three-level inheritance: the original shape that
// surfaced the int8 crash. With the lexer tokens wired and the type
// registered, this now exercises the same layout/codegen paths
// that threeLevelInheritedFieldReadWrite and twoLevelMixedFieldWidths
// already proved correct for int32/int64. Reads cast each level back
// to int64 so the sum doesn't overflow the int8.
TEST(ErrorModelTests, threeLevelMixedFieldWidthsWithInt8) {
    auto src =
        "package test;\n"
        "public class Grandparent {\n"
        "    public int8 gByte;\n"
        "    public Grandparent() { this.gByte = (int8) 0; }\n"
        "}\n"
        "public class Parent extends Grandparent {\n"
        "    public int64 pLong;\n"
        "    public Parent() { this.pLong = 0; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int32 cInt;\n"
        "    public Child() {\n"
        "        this.gByte = (int8) 7;\n"
        "        this.pLong = 12345;\n"
        "        this.cInt = 42;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Child obj = heap Child();\n"
        "        int64 g = (int64) obj.gByte;\n"
        "        int64 c = (int64) obj.cInt;\n"
        "        return obj.pLong + g + c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 12394LL);
}

// Unknown field type produces a clean CAJETA_ERROR_UNKNOWN_TYPE
// instead of a silent null-deref segfault. CajetaType::fromContext
// still returns null for class-or-interface misses because forward
// references (e.g. `permits X` lists where X is declared later) and
// deferred-handler contexts (lambda LVTI params routed to
// NOT_IMPLEMENTED by downstream code) rely on that tolerance. The
// guard sits at visitFieldDeclaration in CajetaLlvmVisitor — early
// enough to capture the offending type-name token from the parser
// context so the error message names what the user actually wrote.
TEST(ErrorModelTests, unknownFieldTypeThrowsCleanError) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public flot32 b;\n"
        "    public Holder() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected cajeta::Exception (unknown type) but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNKNOWN_TYPE");
        EXPECT_NE(e.getMessage().find("flot32"), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain the unresolved field type name";
        EXPECT_NE(e.getMessage().find("b"), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain the field name";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

// Same guard fires when an unknown type appears on a parent class —
// the visit walks each class's field declarations in source order, so
// Parent's `flot32 g` is rejected before Child is even visited.
TEST(ErrorModelTests, unknownInheritedFieldTypeThrowsCleanError) {
    auto src =
        "package test;\n"
        "public class Parent {\n"
        "    public flot32 g;\n"
        "    public Parent() { }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int32 c;\n"
        "    public Child() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Child ch = heap Child();\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected cajeta::Exception (unknown type) but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNKNOWN_TYPE");
        EXPECT_NE(e.getMessage().find("flot32"), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain the unresolved field type name";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

// A class carrying an @Native method is prototyped during prelude codegen —
// *before* visitFieldDeclaration's unknown-type guard would run on the embed
// path — so an unresolved field type on such a class reached struct-layout
// assembly (CajetaClass::generatePrototype's fieldLayoutType) with a null
// CajetaType and segfaulted at `t->getLlvmType()`. The b3 net bring-up hit this
// exact shape with a `bool` field (the canonical primitive name is `boolean`;
// `bool` resolves to no type). The layout-site guard now turns the crash into
// the same clean CAJETA_ERROR_UNKNOWN_TYPE diagnostic, naming the field.
TEST(ErrorModelTests, nativeMethodClassUnknownFieldTypeThrowsCleanError) {
    auto src =
        "package test;\n"
        "public final class Bridge {\n"
        "    public bool flag;\n"
        "    @Native(\"__cajeta_hash_seed\")\n"
        "    public static int64 seed() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected cajeta::Exception (unknown type) but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNKNOWN_TYPE");
        EXPECT_NE(e.getMessage().find("flag"), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not name the offending field";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

// #211 regression: writing to a String-typed field of a regular class used
// to crash codegen because the variable-size-field check (intended for
// CajetaStruct zero-copy types) fired indiscriminately on any class with
// a String field. The fix gates the check on dynamic_pointer_cast<
// CajetaStruct> to limit it to view-struct fields.
TEST(ErrorModelTests, classWithStringField) {
    auto src =
        "package test;\n"
        "public class Holder {\n"
        "    public String message;\n"
        "    public Holder(String s) { this.message = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder(\"hi\");\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// #209: @SuppressLint("uncaught-throws") silences the lint warning on
// the annotated method. The runtime behavior is unchanged — throws still
// propagate, system catch still operates. This test just verifies the
// annotation parses and the body still codegens + runs.
TEST(ErrorModelTests, suppressLintAnnotationSilencesWarning) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException { return 11; }\n"
        "    @SuppressLint(\"uncaught-throws\")\n"
        "    public static int32 run() {\n"
        "        return maybeFail();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Multi-ID array form: @SuppressLint({"id-a", "id-b"}) silences multiple
// rules at once. Today only uncaught-throws exists as a rule, but the
// array-arg parsing must work for the form to be future-proof.
TEST(ErrorModelTests, suppressLintAcceptsArrayArg) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException { return 13; }\n"
        "    @SuppressLint({\"uncaught-throws\", \"unused-local\"})\n"
        "    public static int32 run() {\n"
        "        return maybeFail();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// #210: an uncaught Unrecoverable aborts the process and emits the
// throwable's message + trace to stderr. Death test: spawn the JIT'd
// program and verify it terminates with a SIGABRT-equivalent exit and
// that stderr carries the diagnostic.
TEST(ErrorModelTests, uncaughtUnrecoverableAborts) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        UnrecoverableException u = heap UnrecoverableException(\"contract failure\");\n"
        "        throw u;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    // The throw escapes both the user code and any enclosing try/catch
    // (there is none) — the runtime's __cajeta_throw path detects it as
    // Unrecoverable via the parent-vtable walk and abort()s. EXPECT_DEATH
    // verifies the abort + checks the stderr message.
    EXPECT_DEATH({
        auto jit = CajetaJit::compile(src, "test.D");
        auto fn = jit->lookup<int32_t (*)()>("run");
        fn();
    }, "unrecoverable exception");
}

// Counterpart: an uncaught Recoverable exits cleanly with code 1 (not a
// SIGABRT) and prints the message. The distinction matters for crash
// reporting / debugger interaction — Unrecoverable is the alarm path.
TEST(ErrorModelTests, uncaughtRecoverableExitsClean) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RecoverableException r = heap RecoverableException(\"io failure\");\n"
        "        throw r;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EXIT({
        auto jit = CajetaJit::compile(src, "test.D");
        auto fn = jit->lookup<int32_t (*)()>("run");
        fn();
    }, ::testing::ExitedWithCode(1), "uncaught exception");
}

// #209: a call site wrapped in a try whose catch arm catches the
// declared throw should suppress the [uncaught-throws] warning.
// Captures stderr around the JIT compile to assert on the diagnostic
// output (the warning prints with std::cerr in MethodCallExpression's
// generateCode).
TEST(ErrorModelTests, tryCatchSuppressesUncaughtThrowsWarning) {
    auto src =
        "package test;\n"
        "public class IOException extends RecoverableException {\n"
        "    public IOException() { this.message = \"io\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException { return 11; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        try {\n"
        "            r = maybeFail();\n"
        "        } catch (IOException e) {\n"
        "            r = -1;\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[uncaught-throws]"), std::string::npos)
        << "expected no [uncaught-throws] warning when call is inside "
        << "try { ... } catch (IOException), but got: " << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 11);
}

// #209: catching a SUPERTYPE of the declared throw also suppresses the
// warning. RecoverableException catches IOException (subclass) — same
// rule the runtime applies at throw-time.
TEST(ErrorModelTests, tryCatchSupertypeSuppressesWarning) {
    auto src =
        "package test;\n"
        "public class IOException extends RecoverableException {\n"
        "    public IOException() { this.message = \"io\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException { return 11; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        try {\n"
        "            r = maybeFail();\n"
        "        } catch (RecoverableException e) {\n"
        "            r = -1;\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[uncaught-throws]"), std::string::npos)
        << "expected no [uncaught-throws] warning when call is inside "
        << "try { ... } catch (RecoverableException), but got: " << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 11);
}

// #209: an unrelated catch type (sibling, not supertype) does NOT
// suppress the warning. UnrecoverableException is a sibling of
// RecoverableException under Throwable — it doesn't catch IOException.
// Regression guard for the supertype walk.
TEST(ErrorModelTests, tryCatchUnrelatedTypeStillWarns) {
    auto src =
        "package test;\n"
        "public class IOException extends RecoverableException {\n"
        "    public IOException() { this.message = \"io\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 maybeFail() throws IOException { return 11; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        try {\n"
        "            r = maybeFail();\n"
        "        } catch (UnrecoverableException e) {\n"
        "            r = -1;\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[uncaught-throws]"), std::string::npos)
        << "expected [uncaught-throws] warning when catch type does not "
        << "cover the declared throw, but got no warning";
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 11);
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
        "        Resource r = heap Resource();\n"
        "        return 11;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
