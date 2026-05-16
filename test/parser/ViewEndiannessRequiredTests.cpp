//
// S3.4 — required endianness annotation on view declarations.
//
// Views.md § Endianness: every view declaration must carry one of
// @BigEndian / @LittleEndian / @HostEndian. There is no silent default.
// Layout-pass validation in CajetaView::generatePrototype throws
// CAJETA_ERROR_VIEW_ENDIANNESS_REQUIRED when the annotation is missing.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string viewWithAnnotation(const std::string& annotation) {
    // `annotation` may be empty (no annotation), or one of the three
    // recognized endianness annotations as a complete line.
    return "package test;\n"
         + annotation
         + "public view V {\n"
           "    int32 a;\n"
           "}\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        int32[] bytes = new int32[1];\n"
           "        V v = V(bytes);\n"
           "        v.a = 100;\n"
           "        return v.a;\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(ViewEndiannessRequiredTests, bigEndianAccepted) {
    auto src = viewWithAnnotation("@BigEndian\n");
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

TEST(ViewEndiannessRequiredTests, littleEndianAccepted) {
    auto src = viewWithAnnotation("@LittleEndian\n");
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

TEST(ViewEndiannessRequiredTests, hostEndianAccepted) {
    auto src = viewWithAnnotation("@HostEndian\n");
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

TEST(ViewEndiannessRequiredTests, missingAnnotationRejected) {
    auto src = viewWithAnnotation("");
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}
