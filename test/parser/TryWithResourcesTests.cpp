//
// Try-with-resources tests. v1 scope: the resource variable is in scope
// inside the body (parses + codegens normally). Catch/finally clauses
// continue to work as in the non-resource try form.
//
// Documented limitation: close() does NOT auto-fire at end-of-block today.
// The grammar is wired and the resource init runs; auto-close requires
// constructing method-call AST nodes mechanically (no ANTLR ctx) which is
// awkward enough that it's deferred. Users wanting "close on scope exit"
// should call `r.close()` explicitly at the end of the body for now.
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

// Resource is accessible inside the body. The block can call its methods,
// pass it around, etc. — just like a regular local.
TEST(TryWithResourcesTests, resourceInScopeInsideBody) {
    auto src =
        "package test;\n"
        "public class Stream {\n"
        "    public int32 read() { return 17; }\n"
        "    public void close() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 r;\n"
        "        try (Stream s = new Stream()) {\n"
        "            r = s.read();\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}
