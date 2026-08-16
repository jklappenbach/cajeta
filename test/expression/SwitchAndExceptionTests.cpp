//
// Switch + throw/try/catch + for-init-declaration tests.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class S {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- for-init variable declaration -------------------------------------------



// --- switch ------------------------------------------------------------------






// --- throw + try/catch -------------------------------------------------------






// A method template instantiated ON-REFERENCE at a call site INSIDE a try
// body is codegen'd nested within the caller's body lowering. Its `return`
// must not see the caller's open try frames (the per-function tryFinally/
// tryCatch/loop stacks are detached in Method::generateCode) — before that
// isolation, the instantiation's return emitted the CALLER's try-frame
// unwind (__cajeta_exc_pop with no push in the instantiation), popping the
// live frame so the very next throw in the try body reported "uncaught"
// despite the enclosing catch. Found by nucleo-frame U3 (Column.of<int64>
// inside try); minimal shape pinned here.
TEST(TryCatchTests, methodTemplateCallInsideTryKeepsFrame) {
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    static int64 first<E>(E[] data) {\n"
        "        return (int64) data.count();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64[] a = heap int64[2];\n"
        "        a[0] = 1; a[1] = 2;\n"
        "        try {\n"
        "            int64 n = first<int64>(a);\n"
        "            if (n == 2) {\n"
        "                throw 7;\n"
        "            }\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            return 42;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
