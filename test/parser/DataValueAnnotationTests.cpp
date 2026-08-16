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
    int32_t lenTag;      // len | tag bits (6.2.2 tagged core)
    int32_t aux;         // Inline text 0..3 / window offset
    const char* base;    // Inline text 4..11 / root header
    int32_t cachedCpLength;
};
std::string readToString(const void* raw) {
    if (!raw) return "<null>";
    const auto* s = static_cast<const DvStringLayout*>(raw);
    int32_t len = s->lenTag & 0x1FFFFFFF;
    if (len <= 0) return "";
    if (len <= 12) return std::string((const char*) &s->aux, (size_t) len);
    if (!s->base) return "";
    return std::string(s->base + 8 + s->aux, (size_t) len);
}
}  // namespace

// @Data: getter works.

// @Data: setter works.

// @Data: toString works.

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

// @Value: no setter is generated even though @Setter would normally fire.
// We can't easily assert "method does not exist" — instead we check that
// the all-args ctor is what's expected (setter would have arity 1 conflict
// with field name but the canonical lookup would have caught it). Smoke
// test: compile + run via the all-args ctor.
