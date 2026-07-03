//
// diagnostic-exceptions plan Unit 1: Throwable.toJson() emits the unified
// --diag-format=json schema (superset of {severity,code,message,...}). RED
// until toDiagnostic()/toJson() exist. Content is asserted in-program via
// String.contains so a returned Cajeta String can be checked from the JIT.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string src(const std::string& body) {
    return "package test;\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// 1.1.1: toJson() yields an object carrying at least code + message; the message
// text round-trips.
TEST(DiagnosticJson, toJsonHasCodeAndMessage) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"disk full\");\n"
        "} catch (Exception e) {\n"
        "    String j = e.toJson();\n"
        "    boolean ok = j.contains(\"\\\"code\\\"\")\n"
        "              && j.contains(\"\\\"message\\\"\")\n"
        "              && j.contains(\"disk full\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 1.1.2: code defaults to the canonical type name when no explicit code is set.
TEST(DiagnosticJson, codeDefaultsToTypeName) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"boom\");\n"
        "} catch (Exception e) {\n"
        "    String j = e.toJson();\n"
        "    if (j.contains(\"cajeta.error.Exception\")) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 1.1.3: nested causes serialize as a causeChain array (outer→inner), each with
// its own message.
TEST(DiagnosticJson, causeChainSerializesNestedCauses) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"outer boom\", heap Exception(\"inner boom\"));\n"
        "} catch (Exception e) {\n"
        "    String j = e.toJson();\n"
        "    boolean ok = j.contains(\"\\\"causeChain\\\"\")\n"
        "              && j.contains(\"outer boom\")\n"
        "              && j.contains(\"inner boom\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 1.1.4: the current getStackTrace() output serializes into a frames array
// (address-form at this stage). Capture is on by default in the JIT.
TEST(DiagnosticJson, framesSerializeStackTrace) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"x\");\n"
        "} catch (Exception e) {\n"
        "    String j = e.toJson();\n"
        "    boolean ok = j.contains(\"\\\"frames\\\":[\")\n"
        "              && j.contains(\"nativeAddress\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
