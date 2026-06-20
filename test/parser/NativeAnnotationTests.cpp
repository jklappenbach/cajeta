// Native-deps unit 2 — @Native external-library binding.
//
// @Native(symbol="X", lib="zstd") binds a method to an external native symbol
// (codegen identical to the runtime-symbol form) AND records a native
// requirement (lib-id, symbol) as LLVM named metadata `cajeta.native.reqs` on
// the module, consumed by .cja emission (unit 3) + the resolver (unit 4). The
// bare @Native("symbol") runtime form records nothing.
//
// The bound symbol must resolve for the JIT to materialize, so these tests
// bind to a real runtime symbol (`__cajeta_hash_guid`, (int64,int64)->int64);
// the requirement-recording is independent of which symbol it is.
//
// See agents/cajeta/buildtool/native-deps-plan.md unit 2, spec §2.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
CajetaJit::Options irOpts() {
    CajetaJit::Options o;
    o.captureIr = true;
    return o;
}
} // namespace

// 2.1.1 + 2.1.2 — external-library form emits the extern + records the req.
TEST(NativeAnnotationTests, externalLibBindingRecordsRequirement) {
    auto src =
        "package test;\n"
        "public class Zn {\n"
        "    @Native(symbol = \"__cajeta_hash_guid\", lib = \"zstd\")\n"
        "    public static int64 h(int64 hi, int64 lo);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D", irOpts());
    const std::string& ir = jit->getModuleIr();
    EXPECT_NE(ir.find("__cajeta_hash_guid"), std::string::npos);  // extern symbol
    EXPECT_NE(ir.find("cajeta.native.reqs"), std::string::npos);  // recorded req
    EXPECT_NE(ir.find("zstd"), std::string::npos);                // lib id present
}

// 2.1.1 — the runtime-symbol form (no lib) records no requirement (back-compat).
TEST(NativeAnnotationTests, runtimeSymbolFormRecordsNoRequirement) {
    auto src =
        "package test;\n"
        "public class Zn {\n"
        "    @Native(\"__cajeta_hash_guid\")\n"
        "    public static int64 h(int64 hi, int64 lo);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D", irOpts());
    const std::string& ir = jit->getModuleIr();
    EXPECT_NE(ir.find("__cajeta_hash_guid"), std::string::npos);  // forwarder present
    EXPECT_EQ(ir.find("cajeta.native.reqs"), std::string::npos);  // no req recorded
}
