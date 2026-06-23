//
// Regression: an explicit local-variable type that fails to resolve must be a
// CLEAN diagnostic (CAJETA_ERROR_UNRESOLVED_TYPE), never a SIGSEGV. A null type
// used to flow straight into LocalVariableDeclaration::generateCode and crash at
// the first deref (type->hasValueSemantics()). This guards the fail-loud check
// added in CajetaLlvmVisitor::visitLocalVariableDeclaration. The historical
// trigger was a renamed class (cajeta.gpu.Buffer -> KernelBuffer) still spelled the
// old way in a declaration — see MatrixTests / VectorTypeTests.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

// A local declared at a type name that resolves to nothing throws, not crashes.
TEST(UnresolvedLocalTypeTests, unknownExplicitTypeFailsLoud) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        NoSuchType x = heap NoSuchType(0, 5);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_UNRESOLVED_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNRESOLVED_TYPE");
    }
}

// The stale-rename shape specifically: a parameterized unknown type (the
// renamed class still spelled the old parameterized way) is the same clean
// error, not a crash. Uses an unambiguously nonexistent name so the result
// doesn't depend on which stdlib packages happen to be loaded.
TEST(UnresolvedLocalTypeTests, unknownParameterizedTypeFailsLoud) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        NoSuchBuffer<int32> b = heap NoSuchBuffer<int32>(0, 5);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_UNRESOLVED_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNRESOLVED_TYPE");
    }
}
