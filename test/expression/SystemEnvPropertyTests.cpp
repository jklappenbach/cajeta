// Tests for System.env.{get,set} and System.property.{get,set} —
// OS environment-variable access and process-scoped string properties
// populated by `-Dkey=value` CLI args at binary startup.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

extern "C" {
    const char* __cajeta_env_get(const char* name);
    int32_t     __cajeta_env_set(const char* name, const char* value);
    const char* __cajeta_property_get(const char* name);
    void        __cajeta_property_set(const char* name, const char* value);
    void        __cajeta_property_install(const char* keyEqValue);
}

namespace {

// Capture an int32-returning run() from a cajeta program. The String
// returns aren't used directly by gtest assertions; we route the
// truthiness signal through the int32 return so this stays a simple
// runI32-style fixture.
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE =
    "package test;\n";

} // namespace

// ----- Runtime-level (direct C-API) tests ----------------------------------

TEST(SystemEnvTests, runtimeEnvGetReturnsExistingValue) {
    setenv("CAJETA_TEST_ENV_A", "alpha", /*overwrite=*/1);
    const char* v = __cajeta_env_get("CAJETA_TEST_ENV_A");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "alpha");
}

TEST(SystemEnvTests, runtimeEnvGetReturnsNullForMissing) {
    unsetenv("CAJETA_TEST_ENV_NOTSET");
    EXPECT_EQ(__cajeta_env_get("CAJETA_TEST_ENV_NOTSET"), nullptr);
}

TEST(SystemEnvTests, runtimeEnvSetRoundTrip) {
    __cajeta_env_set("CAJETA_TEST_ENV_B", "beta");
    const char* v = __cajeta_env_get("CAJETA_TEST_ENV_B");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "beta");
    // Reset.
    unsetenv("CAJETA_TEST_ENV_B");
}

TEST(SystemPropertyTests, runtimePropertySetRoundTrip) {
    __cajeta_property_set("cajeta.test.prop.a", "val-a");
    const char* v = __cajeta_property_get("cajeta.test.prop.a");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "val-a");
}

TEST(SystemPropertyTests, runtimePropertyOverwritesPriorValue) {
    __cajeta_property_set("cajeta.test.prop.b", "first");
    __cajeta_property_set("cajeta.test.prop.b", "second");
    const char* v = __cajeta_property_get("cajeta.test.prop.b");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "second");
}

TEST(SystemPropertyTests, runtimePropertyMissingReturnsNull) {
    EXPECT_EQ(__cajeta_property_get("cajeta.test.prop.missing"), nullptr);
}

// `-Dkey=value` parsing — install splits at the first '=' and stores
// the rest as value (empty if no '=' or trailing-`=` form).
TEST(SystemPropertyTests, runtimePropertyInstallParsesKeyEqValue) {
    __cajeta_property_install("cajeta.test.install.full=foobar");
    EXPECT_STREQ(__cajeta_property_get("cajeta.test.install.full"), "foobar");
}

TEST(SystemPropertyTests, runtimePropertyInstallEmptyValue) {
    __cajeta_property_install("cajeta.test.install.empty=");
    EXPECT_STREQ(__cajeta_property_get("cajeta.test.install.empty"), "");
}

TEST(SystemPropertyTests, runtimePropertyInstallFlagOnly) {
    // `-Dflag` (no `=`) sets the property to empty.
    __cajeta_property_install("cajeta.test.install.flag");
    EXPECT_STREQ(__cajeta_property_get("cajeta.test.install.flag"), "");
}

// ----- JIT integration: System.env.{get,set} -------------------------------

TEST(SystemEnvTests, jitGetExistingEnv) {
    setenv("CAJETA_JIT_ENV_X", "hello", /*overwrite=*/1);
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String v = System.env.get(\"CAJETA_JIT_ENV_X\");\n"
        "        if (v == null) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    unsetenv("CAJETA_JIT_ENV_X");
}

TEST(SystemEnvTests, jitGetMissingReturnsNull) {
    unsetenv("CAJETA_JIT_ENV_MISSING");
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String v = System.env.get(\"CAJETA_JIT_ENV_MISSING\");\n"
        "        if (v == null) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(SystemEnvTests, jitSetThenGetSeesValue) {
    unsetenv("CAJETA_JIT_ENV_Y");
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        System.env.set(\"CAJETA_JIT_ENV_Y\", \"v1\");\n"
        "        String v = System.env.get(\"CAJETA_JIT_ENV_Y\");\n"
        "        if (v == null) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    // Verify host-side too.
    EXPECT_STREQ(__cajeta_env_get("CAJETA_JIT_ENV_Y"), "v1");
    unsetenv("CAJETA_JIT_ENV_Y");
}

// ----- JIT integration: System.property.{get,set} --------------------------

TEST(SystemPropertyTests, jitSetThenGetRoundTrip) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        System.property.set(\"cajeta.test.jit.prop\", \"hello\");\n"
        "        String v = System.property.get(\"cajeta.test.jit.prop\");\n"
        "        if (v == null) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    // No cross-process check on __cajeta_property_get here — the JIT links
    // its own copy of the runtime bitcode in addition to the test binary's
    // native runtime object, so they have separate property maps. The
    // cajeta-side roundtrip above is the load-bearing assertion. Binary
    // builds have a single runtime copy, so a `-Dkey=value` arg installs
    // into the same map cajeta code reads — verified manually via
    // samples/.
}

TEST(SystemPropertyTests, jitGetMissingReturnsNull) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String v = System.property.get(\"cajeta.test.jit.prop.missing\");\n"
        "        if (v == null) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `-Dkey=value` CLI args installed via __cajeta_property_install are
// observable from cajeta-side System.property.get IN THE SAME RUNTIME
// COPY. The C-main shim emitted by the binary build calls install at
// startup and cajeta-side `System.property.get` reads the same map.
// The JIT test environment links the runtime twice (native + bitcode)
// so cross-copy visibility doesn't hold there; binary builds have a
// single runtime copy and full cross-source visibility (verified
// manually via samples/, where `./build/run -Dapp.version=2.0` is
// readable from cajeta code).
//
// JIT-mode cajeta install + cajeta read (single-runtime-copy
// roundtrip) is covered by jitSetThenGetRoundTrip above.
