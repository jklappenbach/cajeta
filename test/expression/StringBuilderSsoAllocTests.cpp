//
// Zero-allocation gate for StringBuilder SSO (SSO plan Unit 6; spec 4.3,
// 3.1.1/3.1.2). A <= 64-byte `stack StringBuilder()` build accumulates the
// bytes into the inline `int8[64]` buffer with NO heap allocation on the
// inlined hot path — verified by symbol inspection of the post-AlwaysInline IR
// (no new allocation-counting infrastructure, per spec 4.3):
//   - the inlined build writes through the inline `[64 x i8]` aggregate (SSO
//     active; the bytes live in the stack object, not a heap buffer), and
//   - the build function body contains no `__cajeta_new_array*` / `malloc`
//     call (allocation lives only in the @NoInline `grow`, never reached for
//     a <= 64-byte build).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Extract the IR text of the first `define` whose signature line contains
// `nameFrag`, up to its closing `\n}`. Empty if not found.
std::string funcBody(const std::string& ir, const std::string& nameFrag) {
    size_t pos = 0;
    while ((pos = ir.find("\ndefine", pos)) != std::string::npos) {
        size_t lineEnd = ir.find('\n', pos + 1);
        std::string defLine = ir.substr(pos, lineEnd - pos);
        if (defLine.find(nameFrag) != std::string::npos) {
            size_t bodyEnd = ir.find("\n}", pos);
            return ir.substr(pos, bodyEnd == std::string::npos
                ? std::string::npos : (bodyEnd - pos));
        }
        pos = lineEnd;
    }
    return "";
}

const char* kProgram =
    "package test;\n"
    "import cajeta.lang.StringBuilder;\n"
    "public final class A {\n"
    // <= 64-byte build, no toString: 54 bytes, stays inline, never spills.
    "    public static int32 buildSmall() {\n"
    "        StringBuilder sb = stack StringBuilder();\n"
    "        sb.append(\"0123456789\");\n"  // 10
    "        sb.append(\"0123456789\");\n"  // 20
    "        sb.append(\"0123456789\");\n"  // 30
    "        sb.append(\"0123456789\");\n"  // 40
    "        sb.append(\"0123456789\");\n"  // 50
    "        sb.append(\"0123\");\n"        // 54 (<= 64)
    "        return sb.count();\n"
    "    }\n"
    "    public static int32 run() { return buildSmall(); }\n"
    "}\n";

} // namespace

// 6.1.1 — the <=64-byte build accumulates inline with no allocation; allocation
// is isolated to the cold @NoInline grow spill (unreached for <= 64 bytes).
TEST(StringBuilderSsoAllocTests, SmallBuildIsZeroAllocOnHotPath) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(kProgram, "test.A", opts);

    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 54); // actually built the 54 bytes

    std::string ir = jit->getModuleIr();

    // (a) The whole build path (construct + 6 appends) issues no direct
    //     allocation call — the ctor and appends are non-allocating.
    std::string build = funcBody(ir, "test.A::buildSmall");
    ASSERT_FALSE(build.empty()) << "could not locate buildSmall in IR";
    EXPECT_EQ(build.find("__cajeta_new_array"), std::string::npos)
        << "unexpected heap array allocation in the small-build path:\n" << build;
    EXPECT_EQ(build.find("@malloc"), std::string::npos)
        << "unexpected malloc in the small-build path:\n" << build;

    // (b) The hot append path writes through the inline [64 x i8] buffer (SSO
    //     active) and itself never allocates.
    std::string append = funcBody(ir, "StringBuilder::append(this:pointer,s:");
    ASSERT_FALSE(append.empty()) << "could not locate append in IR";
    EXPECT_NE(append.find("[64 x i8]"), std::string::npos)
        << "expected inline [64 x i8] store path in append:\n" << append;
    EXPECT_EQ(append.find("__cajeta_new_array"), std::string::npos)
        << "append must not allocate inline:\n" << append;

    // (c) The ctor doesn't allocate (no heap backing buffer up front).
    std::string ctor = funcBody(ir, "StringBuilder::StringBuilder(this:");
    ASSERT_FALSE(ctor.empty()) << "could not locate ctor in IR";
    EXPECT_EQ(ctor.find("__cajeta_new_array"), std::string::npos)
        << "ctor must not allocate a backing buffer:\n" << ctor;

    // (d) Positive control: allocation DOES exist, isolated to the cold spill
    //     path (grow), so the gate can detect allocation when present.
    std::string grow = funcBody(ir, "StringBuilder::grow(this:");
    ASSERT_FALSE(grow.empty()) << "could not locate grow in IR";
    EXPECT_NE(grow.find("__cajeta_new_array"), std::string::npos)
        << "grow should hold the (cold) spill allocation:\n" << grow;
}
