// Regression: a class with BOTH a function-typed FIELD and a same-named METHOD
// (the builder idiom — a `handler` closure field + a `handler(fn)` setter).
//
// `b.handler(fn)` must dispatch to the SETTER METHOD, not try to invoke the
// (usually null) field-closure. Before the fix, MethodCallExpression's
// function-typed-field-invocation path greedily matched the field whenever the
// receiver class had a same-named function-typed property — even when a real
// method shadowed it. That single defect caused two visible symptoms on
// `HttpServer.builder()...handler((req) -> ...)`:
//   1. a bare-param lambda arg was evaluated eagerly (before the method's
//      expectedType propagator ran) -> CAJETA_ERROR_TYPE_INFERENCE; and
//   2. with an annotated param, the call invoked the null field-closure slot
//      at runtime -> SIGSEGV.
// The fix: the field-invocation path only fires when no same-named method
// shadows the field (MethodCallExpression.cpp).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runProbe(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Minimal same-class field+method name collision: `handler` field AND setter.
// The call must reach the setter, store the closure, and return `this`.
TEST(BuilderHandlerCollisionTests, fieldAndMethodSameNameDispatchesToMethod) {
    auto src =
        "package test;\n"
        "public class Resp { public int32 code; public Resp(int32 c) { this.code = c; } }\n"
        "public class Builder {\n"
        "    public (int32) -> #Resp handler;\n"          // FIELD named `handler`
        "    public Builder() { this.handler = null; }\n"
        "    public static #Builder make() { return heap Builder(); }\n"
        "    public Builder handler((int32) -> #Resp handler) {\n"  // METHOD `handler`
        "        this.handler = handler; return this;\n"
        "    }\n"
        "    public int32 marker() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Builder b = Builder.make().handler((int32 x) -> heap Resp(x));\n"
        "        return b.marker();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runProbe(src), 7);
}

// Bug #1 end-to-end: a BARE-param lambda arg of a fluent-chained `.handler(...)`
// on the stdlib HttpServerBuilder infers from the setter's formal type.
TEST(BuilderHandlerCollisionTests, fluentBareParamLambdaInfers) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ServerModel;\n"
        "import cajeta.io.net.http.HttpServer;\n"
        "import cajeta.io.net.http.HttpServerBuilder;\n"
        "import cajeta.io.net.http.HttpRequest;\n"
        "import cajeta.io.net.http.HttpResponse;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HttpServerBuilder b = HttpServer.builder()\n"
        "            .bind(\"0.0.0.0:8080\")\n"
        "            .model(ServerModel.sharedPool(16))\n"
        "            .handler((req) -> HttpResponse.of(200));\n"   // BARE param
        "        ServerModel m = b.selectedModel();\n"
        "        if (!m.isSharedPool()) return -1;\n"
        "        if (m.getPoolSize() != 16) return -2;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runProbe(src), 1);
}
