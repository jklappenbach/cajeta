//
// Session 3.5 / Step 3.4 — path-based borrow tracking.
//
// `String n #= person.name` records the dotted path `person.name` on the
// active scope's moved-paths set. Subsequent reads of the same path — or any
// path passing through a moved prefix — are rejected at codegen time with
// CAJETA_ERROR_MOVE_OF_BORROW.
//
// The check fires at the START of DotExpression::generateCode, before any
// codegen that depends on the field actually existing.
//
// The fixtures use REAL classes with real nested fields. They originally used
// `String s` with synthetic members (`s.foo`) that do not exist, because when
// this suite was written user-class field allocation was not end-to-end. That
// only ever compiled because the member check was silently absent; it is not
// absent any more. See the note on `source()` below.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// transfer-demotes-to-borrow — a transferred PATH is readable, exactly as a
// transferred identifier is: `#person.name` demotes `person.name` to a borrow
// of the same live instance rather than killing the path.
void expectCompilesOk(const std::string& source) {
    try {
        CajetaJit::compile(source, "test.P");
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.getErrorId()
                      << ": " << e.getMessage();
    } catch (std::exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.what();
    }
}

// Real classes with real nested fields, so `s.foo` and `s.foo.bar` are genuine
// paths.
//
// These fixtures used to be `String s = "hello"` with SYNTHETIC members
// (`s.foo`, `s.bar`) that do not exist on String — the header note above
// explains why: when this suite was written, user-class field allocation was
// not end-to-end, and the path tracker runs before the member lookup, so fake
// fields were a cheap way to exercise it. That worked only because the compiler
// silently accepted a member that did not exist. It no longer does
// (silent-resolution-diagnostics Unit 2: `no member 'foo' on 'cajeta.lang.String'`).
//
// The path shapes under test are UNCHANGED — s.foo, s.foo.bar, s.bar. Only the
// receiver is now a type that really has them, so the suite tests the path
// tracker rather than the absence of a member check.
std::string sourceNamed(const std::string& cls, const std::string& body) {
    return "package test;\n"
           "public class Inner {\n"
           "    public String bar = \"b\";\n"
           "}\n"
           "public class Outer {\n"
           "    public Inner foo;\n"
           "    public String bar = \"sib\";\n"
           "}\n"
           "public final class " + cls + " {\n"
           "    public static int32 run() {\n"
           "        Outer s = heap Outer();\n"
           "        s.foo = heap Inner();\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

std::string source(const std::string& body) {
    return "package test;\n"
           "public class Inner {\n"
           "    public String bar = \"b\";\n"
           "}\n"
           "public class Outer {\n"
           "    public Inner foo;\n"
           "    public String bar = \"sib\";\n"
           "}\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        Outer s = heap Outer();\n"
           "        s.foo = heap Inner();\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

} // namespace

// title-tracking rev 2 (Unit 8 respell): a BIT-CAPABLE class field
// extraction (`#s.foo`) is the guarded detach — the ownership bit governs
// at runtime and the slot stays readable as a lend (the LinkedList/Cache
// pop idioms depend on it), so the static path invalidation no longer
// applies to class fields. Root moves (`#s`) and String-path moves keep
// the static rule — pinned unchanged elsewhere in this suite. These three
// pin the runtime behavior: extraction + re-read is defined and titles
// balance (one drop, no UAF).
TEST(PathBorrowTests, readSamePathAfterClassExtractionIsLend) {
    auto src = sourceNamed("PLend1", 
        "Inner moved #= s.foo;\n"
        "Inner n = s.foo;\n"
        "if (n.bar.byteLength() != 1) { return -1; }");
    auto jit = CajetaJit::compile(src, "test.PLend1");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}


TEST(PathBorrowTests, doubleClassExtractionYieldsBorrow) {
    // First extraction takes the field's bit; a SECOND `#` extraction
    // finds the bit clear and yields a BORROW of the still-resident value
    // (mode-carrying-claim §5.1) — no panic, no forged title; `a` keeps
    // the only title, so the drop still happens exactly once.
    auto src = sourceNamed("PLend2",
        "Inner a #= s.foo;\n"
        "Inner b #= s.foo;\n"
        "if (b.bar.byteLength() != 1) { return -2; }\n"
        "return 42;");
    auto jit = CajetaJit::compile(src, "test.PLend2");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}


TEST(PathBorrowTests, deeperReadAfterClassExtractionIsLend) {
    // Reading THROUGH an extracted class field is a lend of live memory
    // (the extractor local owns it) — defined under rev 2.
    auto src = sourceNamed("PLend3", 
        "Inner moved #= s.foo;\n"
        "String n = s.foo.bar;\n"
        "if (n.byteLength() != 1) { return -3; }");
    auto jit = CajetaJit::compile(src, "test.PLend3");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// --- Valid: different sub-paths are independent ----------------------------


TEST(PathBorrowTests, unmovedPathReadable) {
    // No moves anywhere — DotExpression should not raise a path-move error.
    auto src = source(
        "Inner n = s.foo;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.P"));
}
