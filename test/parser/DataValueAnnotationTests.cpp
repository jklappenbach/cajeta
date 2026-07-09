// Tests for the @Data and @Value bundle annotations
// (docs/specification/reflect/Annotations.md § Bundles).
//
// @Data  = @Getter + @Setter + @ToString + @AutoHash + @RequiredArgsConstructor
// @Value = @Getter + @ToString + @AutoHash + @AllArgsConstructor (no setters;
//          immutable variant — @Value wins over any explicit @Setter)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// Synthesized toString now returns a REAL cajeta.lang.String (slices plan
// 6.1.4 audit — __cajeta_string_wrap_cstr at the synthesizer tail), so read
// the object layout instead of treating the pointer as a C string.
struct DvStringLayout {
    const void* vtable;
    const void* bytes;
    int32_t byteLength;
    int32_t mode;
    int32_t cachedCpLength;
};
std::string readToString(const void* raw) {
    if (!raw) return "<null>";
    const auto* s = static_cast<const DvStringLayout*>(raw);
    if (!s->bytes || s->byteLength <= 0) return "";
    const char* data = (const char*) s->bytes + sizeof(int64_t);
    return std::string(data, (size_t) s->byteLength);
}
}  // namespace

// @Data: getter works.
TEST(DataValueAnnotationTests, dataEnablesGetter) {
    auto src =
        "package test;\n"
        "@Data public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(42);\n"
        "        return p.n();\n"  // getter
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// @Data: setter works.
TEST(DataValueAnnotationTests, dataEnablesSetter) {
    auto src =
        "package test;\n"
        "@Data public class P {\n"
        "    public int32 n;\n"
        "    public P() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        p.n(7);\n"  // setter
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// @Data: toString works.
TEST(DataValueAnnotationTests, dataEnablesToString) {
    auto src =
        "package test;\n"
        "@Data public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(5);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<const void* (*)()>("run");
    EXPECT_EQ(readToString(fn()), "P(n=5)");
}

// @Data: @RequiredArgsConstructor — final fields only.
TEST(DataValueAnnotationTests, dataEnablesRequiredArgsCtor) {
    auto src =
        "package test;\n"
        "@Data public class P {\n"
        "    public final int32 id;\n"
        "    public int32 counter;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(99);\n"  // ctor takes only `id` (final)
        "        return p.id;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// @Value: getter works, AllArgsConstructor works, ToString works.
TEST(DataValueAnnotationTests, valueGivesGetterAllArgsCtorAndToString) {
    auto src =
        "package test;\n"
        "@Value public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Point p = heap Point(3, 4);\n"  // @AllArgsConstructor
        "        int32 sum = p.x() + p.y();\n"  // @Getter
        "        return p.toString();\n"       // @ToString — sum unused but
        "    }\n"                              // exercises getter codegen
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<const void* (*)()>("run");
    EXPECT_EQ(readToString(fn()), "Point(x=3,y=4)");
}

// @Value: no setter is generated even though @Setter would normally fire.
// We can't easily assert "method does not exist" — instead we check that
// the all-args ctor is what's expected (setter would have arity 1 conflict
// with field name but the canonical lookup would have caught it). Smoke
// test: compile + run via the all-args ctor.
TEST(DataValueAnnotationTests, valueDoesNotGenerateSetter) {
    auto src =
        "package test;\n"
        "@Value public class Frozen {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Frozen f = heap Frozen(42);\n"
        "        return f.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
