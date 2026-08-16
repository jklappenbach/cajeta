//
// silent-resolution diagnostics — Unit 3, initializers are checked.
// Plan: agents/silent-resolution-diagnostics-plan.md (spec 3.1-3.3).
//
// An initializer is a value position like any other, but it was never
// resolution- or type-checked: `static int32 x = NoSuchType.nope();` and
// `static int32 x = "hello";` both compiled CLEAN. The Unit 1 backstop does
// not catch them — it guards `return` / conditions, and an initializer never
// passes through either.
//
//   3.1.1 unresolvable call in an initializer  -> located compile error
//   3.1.2 type-mismatched initializer          -> located type error
//   3.1.3 both hold for instance fields and locals, not just statics
//   3.1.4 every LEGAL initializer shape still compiles (over-rejection guard)
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

cajeta::Exception expectThrows(const std::string& src) {
    try {
        runI32(src);
    } catch (cajeta::Exception& e) {
        return e;
    }
    ADD_FAILURE() << "expected the initializer to fail the compile";
    return cajeta::Exception("no error was thrown", "NONE");
}

} // namespace

// 3.1.1: a STATIC field initializer calling a type that does not exist.
TEST(InitializerCheckTests, staticFieldUnresolvableCallIsAnError) {
    auto e = expectThrows(
        "package test;\n"
        "public final class D {\n"
        "    static int32 x = NoSuchType.nope();\n"
        "    public static int32 run() { return D.x; }\n"
        "}\n");
    EXPECT_TRUE(e.hasLocation()) << "must carry a source span";
    EXPECT_NE(e.getMessage().find("NoSuchType"), std::string::npos)
        << "must name the unresolvable type: " << e.getMessage();
}

// 3.1.2: a STATIC field initializer whose type does not match the declaration.
TEST(InitializerCheckTests, staticFieldTypeMismatchIsAnError) {
    auto e = expectThrows(
        "package test;\n"
        "public final class D {\n"
        "    static int32 x = \"hello\";\n"
        "    public static int32 run() { return D.x; }\n"
        "}\n");
    EXPECT_TRUE(e.hasLocation());
}

// 3.1.3a: the same, on an INSTANCE field.

// 3.1.3b: the same, on a LOCAL initializer.

// 3.1.4: OVER-REJECTION GUARD. Every legal initializer shape must still
// compile: literal, static call, ctor call, cast, and a field read. If this
// goes red, the checks above are too aggressive -- that is the real danger
// here, not under-rejection.
TEST(InitializerCheckTests, legalInitializersStillCompile) {
    int32_t got = runI32(
        "package test;\n"
        "public class Box {\n"
        "    public int32 n = 3;\n"
        "    public Box() { }\n"
        "    public static int32 five() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    static int32 lit = 1;\n"                       // literal
        "    static int32 viaCall = Box.five();\n"          // static call
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"                     // ctor call
        "        int32 fromField = b.n;\n"                  // field read
        "        int32 casted = (int32) 2.0;\n"             // cast
        "        return D.lit + D.viaCall + fromField + casted;\n"  // 1+5+3+2
        "    }\n"
        "}\n");
    EXPECT_EQ(got, 11);
}
