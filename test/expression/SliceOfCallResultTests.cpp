// The window form `base[a:b]` over a call result: the base's type is the call's
// resolved return type, and a base that is not an array or Slice<T> reports
// CAJETA_ERROR_SLICE_BASE at the base's source position.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "class Buf {\n"
    "    int8[] data;\n"
    "    int32 n;\n"
    "    public Buf() {\n"
    "        this.data = heap int8[6];\n"
    "        int32 i = 0;\n"
    "        while (i < 6) { this.data[i] = (int8) (10 + i); i = i + 1; }\n"
    "        this.n = 6;\n"
    "    }\n"
    "    public int8[] array() { return this.data; }\n"
    "    public int32 size() { return this.n; }\n"
    "    public Slice<int8> window() { return this.data[0:4]; }\n"
    "    public static #int8[] fresh() { return heap int8[4]; }\n"
    "}\n";

int32_t runI32(const std::string& body) {
    std::string src = std::string(kPrelude)
        + "public final class S {\n"
          "    public static int32 run() {\n"
        + body
        + "    }\n"
          "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    return jit->lookup<int32_t (*)()>("run")();
}

struct Diag {
    std::string id;
    int line = 0;
};

Diag diagnosticFor(const std::string& body) {
    std::string src = std::string(kPrelude)
        + "public final class S {\n"
          "    public static int32 run() {\n"
        + body
        + "    }\n"
          "}\n";
    Diag d;
    try {
        CajetaJit::compile(src, "test.S");
        d.id = "<compiled>";
    } catch (cajeta::Exception& e) {
        d.id = e.getErrorId();
        d.line = e.getLine();
    }
    return d;
}

}  // namespace

TEST(SliceOfCallResultTests, sliceOfAnArrayReturningCall) {
    EXPECT_EQ(runI32("        Buf b = heap Buf();\n"
                     "        Slice<int8> s = b.array()[1:4];\n"
                     "        return (int32) s.count() * 100 + (int32) s[0] + (int32) s[2];\n"),
              300 + 11 + 13);
}

TEST(SliceOfCallResultTests, sliceOfACallResultInsideAnExpression) {
    EXPECT_EQ(runI32("        Buf b = heap Buf();\n"
                     "        return (int32) b.array()[2:6].count();\n"), 4);
}

TEST(SliceOfCallResultTests, sliceOfASliceReturningCall) {
    EXPECT_EQ(runI32("        Buf b = heap Buf();\n"
                     "        Slice<int8> s = b.window()[1:3];\n"
                     "        return (int32) s.count() * 100 + (int32) s[1];\n"),
              200 + 12);
}

TEST(SliceOfCallResultTests, sliceOfALocalStillWorks) {
    EXPECT_EQ(runI32("        Buf b = heap Buf();\n"
                     "        int8[] a = b.array();\n"
                     "        Slice<int8> s = a[1:4];\n"
                     "        return (int32) s.count() * 100 + (int32) s[0];\n"),
              300 + 11);
}

TEST(SliceOfCallResultTests, sliceOfAScalarCallIsRejectedWithAPosition) {
    Diag d = diagnosticFor("        Buf b = heap Buf();\n"
                           "        Slice<int8> s = b.size()[0:1];\n"
                           "        return 0;\n");
    EXPECT_EQ(d.id, "CAJETA_ERROR_SLICE_BASE");
    EXPECT_EQ(d.line, 19);
}

TEST(SliceOfCallResultTests, sliceOfAScalarLocalIsRejectedWithAPosition) {
    Diag d = diagnosticFor("        int32 x = 5;\n"
                           "        Slice<int8> s = x[0:1];\n"
                           "        return 0;\n");
    EXPECT_EQ(d.id, "CAJETA_ERROR_SLICE_BASE");
    EXPECT_EQ(d.line, 19);
}

TEST(SliceOfCallResultTests, sliceOfAnOwnedCallResultIsRejected) {
    Diag d = diagnosticFor("        Slice<int8> s = Buf.fresh()[0:2];\n"
                           "        return (int32) s.count();\n");
    EXPECT_EQ(d.id, "CAJETA_ERROR_SLICE_OF_OWNED_TEMP");
    EXPECT_EQ(d.line, 18);
}

TEST(SliceOfCallResultTests, sliceOfAnOwnedResultBoundToALocalWorks) {
    EXPECT_EQ(runI32("        int8[] a #= Buf.fresh();\n"
                     "        Slice<int8> s = a[1:3];\n"
                     "        return (int32) s.count();\n"), 2);
}
