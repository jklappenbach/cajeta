//
// View endianness default — a view declaration with no endianness annotation
// defaults to @HostEndian.
//
// Views.md § Endianness: @BigEndian / @LittleEndian / @HostEndian are all
// accepted; omitting the annotation is equivalent to @HostEndian (host byte
// order, no bswap). The explicit-vs-implicit distinction is still tracked
// (CajetaView::hasExplicitEndianness) for nested-view inheritance — a nested
// view with no annotation inherits its outer's order rather than forcing host.
//
// (Was ViewEndiannessRequiredTests: the original design required the
// annotation and rejected its absence. That guardrail was lifted in favor of
// an ergonomic host-order default.)
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
           "        int32[] bytes = heap int32[1];\n"
           "        V v = V(bytes);\n"
           "        v.a = 100;\n"
           "        return v.a;\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(ViewEndiannessDefaultTests, bigEndianAccepted) {
    EXPECT_EQ(runI32(viewWithAnnotation("@BigEndian\n")), 100);
}

TEST(ViewEndiannessDefaultTests, littleEndianAccepted) {
    EXPECT_EQ(runI32(viewWithAnnotation("@LittleEndian\n")), 100);
}

TEST(ViewEndiannessDefaultTests, hostEndianAccepted) {
    EXPECT_EQ(runI32(viewWithAnnotation("@HostEndian\n")), 100);
}

// No annotation now compiles (previously rejected) and round-trips.
TEST(ViewEndiannessDefaultTests, missingAnnotationCompiles) {
    EXPECT_EQ(runI32(viewWithAnnotation("")), 100);
}

// No annotation defaults to host order: write a value through a @BigEndian
// view, then read the same buffer through an unannotated view. The unannotated
// view reads in host (little, on x86_64/aarch64) order, so it observes the
// byte-reversed value — exactly as an explicit @HostEndian view would
// (cf. EndianAlignTests.bigEndianStorageVisibleAsReversedBytes).
TEST(ViewEndiannessDefaultTests, missingAnnotationDefaultsToHost) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Be {\n"
        "    int32 val;\n"
        "}\n"
        "public view Plain {\n"          // no annotation → @HostEndian
        "    int32 val;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[1];\n"
        "        Be be = Be(bytes);\n"
        "        be.val = 16909060;\n"     // 0x01020304 stored big-endian: 01 02 03 04
        "        Plain p = Plain(bytes);\n"
        "        return p.val;\n"           // host (little) order read: 04 03 02 01 = 0x04030201
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0x04030201);
}
