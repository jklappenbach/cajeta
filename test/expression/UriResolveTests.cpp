// Tests for cajeta.io.net.uri.Uri reference resolution + builder +
// recomposition — plan item NET-6.4 (RFC 3986 §5 reference resolution,
// §5.3 recomposition, and the fluent Uri/UriBuilder).
//
// Two layers of coverage:
//
//   1. The RFC 3986 §5.4 NORMATIVE golden table (loaded from the
//      NET-13.2 corpus, test/net/golden/uri/rfc3986-resolution.vectors):
//      for every <base> | <reference> | <resolved> row we JIT a tiny
//      cajeta program that does
//          Uri.resolve(Uri.parse(base), reference).toString()
//      and assert it equals <resolved>. This pins resolve + the §5.2.4
//      remove_dot_segments + §5.2.3 merge + §5.3 recompose path
//      end-to-end against the spec's own examples.
//
//   2. Focused builder + toString round-trip cases (the API surface in
//      docs/Net.md).
//
// Harness mirrors UriParseTests: compile a cajeta source through the
// JIT, call run() -> int32 returning 1 on match / 0 otherwise.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include "net/GoldenVectors.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Escape a string for embedding inside a cajeta double-quoted literal.
// The §5.4 corpus uses only printable ASCII (no quotes/backslashes), but
// escape defensively so a future corpus row can't silently corrupt the
// generated source.
std::string lit(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.uri.Uri;\n"
           "import cajeta.io.net.uri.UriBuilder;\n"
           "import cajeta.io.net.uri.MalformedUriException;\n"
           "public final class U {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- RFC 3986 §5.4 normative reference-resolution table ----------------
// → UriTests.rfc3986ReferenceResolutionVectors (plan acceptance)
//
// One gtest per golden row keeps a failure pointing at the exact
// (base, reference) pair that diverged.

TEST(UriResolveTests, rfc3986ReferenceResolutionVectors) {
    auto vectors = cajeta_golden::uriResolutionVectors();
    ASSERT_GE(vectors.size(), 40u)
        << "expected the full §5.4.1 + §5.4.2 normative table";

    // Compile ONCE for the whole table. Each JIT compile recompiles the full
    // stdlib prelude (~8 s), so one-compile-per-vector made this a ~40 × 8 s
    // ≈ 320 s test that tripped the per-test timeout and looked like a hang.
    //
    // We emit ONE module with a helper method per vector plus a run()
    // dispatcher that returns the 1-based index of the first mismatch (0 = all
    // pass) — so a failure still pinpoints the exact (base, reference). Each
    // helper mirrors the function-scoped owned-local shape of the original
    // per-vector source. (Running the full table this way also surfaced a real
    // empty-reference bug in Uri.resolve that the old per-vector timeout hid.)
    std::string methods;
    std::string dispatch;
    for (size_t i = 0; i < vectors.size(); i++) {
        const auto& v = vectors[i];
        std::string id = std::to_string(i);
        methods +=
            "    private static int32 check" + id + "() {\n"
            "        Uri base = Uri.parse(" + lit(v.base) + ");\n"
            "        Uri r = Uri.resolve(base, " + lit(v.reference) + ");\n"
            "        return r.toString().equals(" + lit(v.resolved)
            + ") ? 1 : 0;\n"
            "    }\n";
        dispatch +=
            "        if (U.check" + id + "() == 0) { return "
            + std::to_string(i + 1) + "; }\n";
    }
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.uri.Uri;\n"
        "public final class U {\n"
        + methods +
        "    public static int32 run() {\n"
        + dispatch +
        "        return 0;\n"
        "    }\n"
        "}\n";

    int32_t rc = runI32(src);
    if (rc != 0) {
        const auto& v = vectors[rc - 1];
        FAIL() << "resolve(" << v.base << ", '" << v.reference
               << "') should be " << v.resolved << " (vector #" << rc << ")";
    }
}

// --- §5.3 recomposition round-trips (toString is parse's inverse) ------

TEST(UriResolveTests, toStringRoundTripsFullUri) {
    // Explicit port present in the input → serialized back verbatim.
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\");\n"
        "return u.toString().equals("
        "\"https://u:p@h.test:8443/a/b?x=1&y=2#frag\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, toStringOmitsSchemeDefaultPort) {
    // http default port 80 is NOT explicit → must not appear in output.
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"http://h.test/p\");\n"
        "return u.toString().equals(\"http://h.test/p\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, toStringRebracketsIpv6Literal) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"http://[::1]:8080/x\");\n"
        "return u.toString().equals(\"http://[::1]:8080/x\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, toStringRoundTripsAuthorityless) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"mailto:a@b.test\");\n"
        "return u.toString().equals(\"mailto:a@b.test\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, toStringRoundTripsEmptyQuery) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.parse(\"http://h.test/p?\");\n"
        "return u.toString().equals(\"http://h.test/p?\") ? 1 : 0;")), 1);
}

// --- Builder -----------------------------------------------------------

TEST(UriResolveTests, builderAssemblesFullUri) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.builder()\n"
        "    .scheme(\"https\").host(\"h.test\").port(8443)\n"
        "    .path(\"/a/b\").query(\"x=1\").fragment(\"frag\")\n"
        "    .build();\n"
        "return u.toString().equals("
        "\"https://h.test:8443/a/b?x=1#frag\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, builderLowercasesScheme) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.builder().scheme(\"HTTPS\").host(\"h.test\")\n"
        "    .path(\"/\").build();\n"
        "return u.getScheme().equals(\"https\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, builderBracketsIpv6Host) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.builder().scheme(\"http\").host(\"::1\").port(80)\n"
        "    .path(\"/\").build();\n"
        "return u.toString().equals(\"http://[::1]:80/\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, builderOmitsUnsetPort) {
    EXPECT_EQ(runI32(makeSource(
        "Uri u = Uri.builder().scheme(\"http\").host(\"h.test\")\n"
        "    .path(\"/p\").build();\n"
        "return u.toString().equals(\"http://h.test/p\") ? 1 : 0;")), 1);
}

// --- resolve spot-checks (independent of the golden loop) --------------

TEST(UriResolveTests, resolveRelativeDotDotSpotCheck) {
    EXPECT_EQ(runI32(makeSource(
        "Uri base = Uri.parse(\"http://a/b/c/d;p?q\");\n"
        "Uri r = Uri.resolve(base, \"../../g\");\n"
        "return r.toString().equals(\"http://a/g\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, resolveAbsoluteReferenceKeepsItsScheme) {
    EXPECT_EQ(runI32(makeSource(
        "Uri base = Uri.parse(\"http://a/b/c/d;p?q\");\n"
        "Uri r = Uri.resolve(base, \"https://other.test/x\");\n"
        "return r.toString().equals(\"https://other.test/x\") ? 1 : 0;")), 1);
}

TEST(UriResolveTests, resolveEmptyReferenceKeepsBasePathAndQuery) {
    EXPECT_EQ(runI32(makeSource(
        "Uri base = Uri.parse(\"http://a/b/c/d;p?q\");\n"
        "Uri r = Uri.resolve(base, \"\");\n"
        "return r.toString().equals(\"http://a/b/c/d;p?q\") ? 1 : 0;")), 1);
}
