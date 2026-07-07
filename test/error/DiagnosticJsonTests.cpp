//
// diagnostic-exceptions plan Unit 1: Throwable.toJson() emits the unified
// --diag-format=json schema (superset of {severity,code,message,...}). RED
// until toDiagnostic()/toJson() exist. Content is asserted in-program via
// String.contains so a returned Cajeta String can be checked from the JIT.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

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

// 2.1.1: a user exception's public fields serialize into `fields` with correct
// names + typed values (via reflection), with no per-type serialization code.
TEST(DiagnosticJson, fieldsSerializeUserExceptionFields) {
    std::map<std::string, std::string> sources;
    sources["test.IoError"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class IoError extends Exception {\n"
        "    public String path;\n"
        "    public int32 status;\n"
        "    public IoError(#String message, #String path, int32 status) {\n"
        "        this.message = message;\n"
        "        this.cause = 0;\n"
        "        this.path = path;\n"
        "        this.status = status;\n"
        "    }\n"
        "}\n";
    sources["test.S"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap IoError(\"open failed\", \"/etc/passwd\", 13);\n"
        "        } catch (Exception e) {\n"
        "            String j = e.toJson();\n"
        "            boolean ok = j.contains(\"\\\"fields\\\"\")\n"
        "                      && j.contains(\"\\\"path\\\":\\\"/etc/passwd\\\"\")\n"
        "                      && j.contains(\"\\\"status\\\":13\");\n"
        "            if (ok) { return 1; }\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 2.1.3: category affordances default false and serialize; a subclass override
// flips them. 2.1.4: remediation round-trips when hint()/docUrl() are set, and
// is absent otherwise.
TEST(DiagnosticJson, categoryDefaultsAndRemediationAbsent) {
    auto jit = CajetaJit::compile(src(
        "try {\n"
        "    throw heap Exception(\"plain\");\n"
        "} catch (Exception e) {\n"
        "    String j = e.toJson();\n"
        "    boolean ok = j.contains(\"\\\"category\\\":{\\\"retryable\\\":false\")\n"
        "              && j.contains(\"\\\"userActionable\\\":false\")\n"
        "              && !j.contains(\"\\\"remediation\\\"\");\n"
        "    if (ok) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "return -1;"), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(DiagnosticJson, categoryOverrideAndRemediationRoundTrip) {
    std::map<std::string, std::string> sources;
    sources["test.Flaky"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class Flaky extends Exception {\n"
        "    public Flaky(#String message) { this.message = message; this.cause = null; }\n"
        "    public boolean isRetryable() { return true; }\n"
        "    public boolean isTransient() { return true; }\n"
        "    public Optional<String> hint() { return stack Optional<String>(true, \"retry with backoff\"); }\n"
        "    public Optional<String> docUrl() { return stack Optional<String>(true, \"https://cajeta.dev/e/flaky\"); }\n"
        "}\n";
    sources["test.S"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap Flaky(\"timeout\");\n"
        "        } catch (Exception e) {\n"
        "            String j = e.toJson();\n"
        "            boolean ok = j.contains(\"\\\"retryable\\\":true\")\n"
        "                      && j.contains(\"\\\"transient\\\":true\")\n"
        "                      && j.contains(\"\\\"remediation\\\":{\")\n"
        "                      && j.contains(\"\\\"hint\\\":\\\"retry with backoff\\\"\")\n"
        "                      && j.contains(\"https://cajeta.dev/e/flaky\");\n"
        "            if (ok) { return 1; }\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 2.1.2: @DiagnosticCode("...") on a type sets its JSON `code`; without it, code
// stays the canonical type name (default covered by codeDefaultsToTypeName).
TEST(DiagnosticJson, diagnosticCodeAnnotationOverridesDefault) {
    std::map<std::string, std::string> sources;
    sources["test.DiskFull"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.DiagnosticCode;\n"
        "@DiagnosticCode(\"CAJETA_ERR_IO_DISK_FULL\")\n"
        "public final class DiskFull extends Exception {\n"
        "    public DiskFull(#String message) { this.message = message; this.cause = 0; }\n"
        "}\n";
    sources["test.S"] =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap DiskFull(\"no space left\");\n"
        "        } catch (Exception e) {\n"
        "            String j = e.toJson();\n"
        "            boolean ok = j.contains(\"\\\"code\\\":\\\"CAJETA_ERR_IO_DISK_FULL\\\"\")\n"
        "                      && j.contains(\"no space left\");\n"
        "            if (ok) { return 1; }\n"
        "            return 0;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
