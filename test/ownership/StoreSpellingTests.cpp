// The ownership store has one spelling (spec Ownership §5.3): `x #= v`.
// A move expression as the right side of a plain `=` is rejected.
//
// Measured 2026-09-08 on 0.27.0: `Probe b = #a;` compiles and carries
// the meaning of `b #= a` — the title moves (a second `#a` is
// CAJETA_ERROR_MOVE_OF_BORROW, and the drop fires at b's scope end).
// Julian decided 2026-09-08 to remove the `= #v` spelling and keep
// `#=` as the only ownership store.
//
// No shipped diagnostic covers this shape; the expected code below
// names the intent. Adjust it if the fix names a different code.
//
// RED until the single-spelling rule lands in the ownership
// consolidation.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

}  // namespace

// `= #v` at a declaration is rejected; the store spelling is `#=`.
TEST(StoreSpellingTests, DISABLED_moveExprInDeclarationStoreRejected) {
    const char* src =
        "package test;\n"
        "public class Probe { public int32 id;"
        " public Probe(int32 id) { this.id = id; } }\n"
        "public final class D {\n"
        "    public static void run() {\n"
        "        Probe a = heap Probe(1);\n"
        "        Probe b = #a;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_MOVE_IN_PLAIN_STORE");
}

// `= #v` at a reassignment is rejected the same way.
TEST(StoreSpellingTests, DISABLED_moveExprInReassignmentStoreRejected) {
    const char* src =
        "package test;\n"
        "public class Probe { public int32 id;"
        " public Probe(int32 id) { this.id = id; } }\n"
        "public final class D {\n"
        "    public static void run() {\n"
        "        Probe a = heap Probe(1);\n"
        "        Probe b = heap Probe(2);\n"
        "        b = #a;\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_MOVE_IN_PLAIN_STORE");
}
