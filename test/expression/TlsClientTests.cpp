// NET-5.2 — TlsClient (Cajeta surface over the __cajeta_tls_* engine), driven
// through the JIT. First: confirm the JIT resolves the native engine symbols at
// @Native call sites (they live in libcajeta_lib, not the embedded bitcode).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    @Native(\"__cajeta_tls_ctx_new\")\n"
        "    public static pointer ctxNew(int32 isServer) { }\n"
        "    @Native(\"__cajeta_tls_ctx_free\")\n"
        "    public static void ctxFree(pointer ctx) { }\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Resolution probe: a client TLS context can be created + freed through the
// @Native engine binding (proves the JIT links the native __cajeta_tls_* syms).
TEST(TlsClientTests, jitResolvesEngineSymbols) {
    EXPECT_EQ(runI32(
        "        pointer ctx = ctxNew(0);\n"
        "        if (ctx == null) { return -1; }\n"
        "        ctxFree(ctx);\n"
        "        return 1;\n"), 1);
}
