// NET-7.1 — cajeta.io.net.http.HttpRequest / HttpResponse tests.
//
// The HTTP message types are pure data (request-line / status-line
// triple + a Headers map + an in-memory body handle), no I/O, so they
// test directly over the JIT: each test compiles a small Cajeta
// `run()` that builds a message and returns an int32 sentinel —
// positive on success, a distinct negative per failed sub-check (the
// same convention HttpHeadersTests / UriParseTests / JsonReaderTests
// use).
//
// Pins the NET-7.1 deliverable: "HttpRequest / HttpResponse types:
// method, target, version, status, reason, headers, body handle.
// Immutable-ish builders." (plan/cajeta-net-plan.md, Phase 7).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the HTTP message types, the
// header map, the URI type, and String. The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.Headers;\n"
        "import cajeta.io.net.uri.Uri;\n"
        "import cajeta.io.net.http.HttpRequest;\n"
        "import cajeta.io.net.http.HttpResponse;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- HttpRequest -------------------------------------------------------

// A default request is GET / HTTP/1.1 with no body.
TEST(HttpMessageTests, requestDefaults) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/\");\n"
        "if (!r.getMethod().equals(\"GET\")) return -1;\n"
        "if (!r.getTarget().equals(\"/\")) return -2;\n"
        "if (!r.getVersion().equals(\"HTTP/1.1\")) return -3;\n"
        "if (r.hasBody) return -4;\n"
        "if (r.bodyLength() != 0) return -5;\n"
        "return 1;"), 1);
}

// of() takes the method token verbatim (case-sensitive) and the
// target as-is.
TEST(HttpMessageTests, requestOfPreservesMethodAndTarget) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.of(\"DELETE\", \"/items/7\");\n"
        "if (!r.getMethod().equals(\"DELETE\")) return -1;\n"
        // method is case-sensitive: lowercase must NOT match.
        "if (r.getMethod().equals(\"delete\")) return -2;\n"
        "if (!r.getTarget().equals(\"/items/7\")) return -3;\n"
        "return 1;"), 1);
}

// post() yields a POST request; the body() mutator attaches an
// in-memory body and flips hasBody.
TEST(HttpMessageTests, requestPostWithBody) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.post(\"/submit\");\n"
        "if (!r.getMethod().equals(\"POST\")) return -1;\n"
        "int8[] payload = heap int8[3];\n"
        "payload[0] = (int8) 104;\n"   // 'h'
        "payload[1] = (int8) 105;\n"   // 'i'
        "payload[2] = (int8) 33;\n"    // '!'
        "r.body(payload, 3);\n"
        "if (!r.hasBody) return -2;\n"
        "if (r.bodyLength() != 3) return -3;\n"
        "if (r.body[0] != (int8) 104) return -4;\n"
        "if (r.body[2] != (int8) 33) return -5;\n"
        "return 1;"), 1);
}

// header() appends (multi-value); the request's Headers map is the
// case-insensitive, order-preserving NET-7.2 map.
TEST(HttpMessageTests, requestHeadersAreCaseInsensitive) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/\");\n"
        "r.header(\"Accept\", \"text/html\");\n"
        "r.header(\"Accept\", \"application/json\");\n"
        "Headers h = r.getHeaders();\n"
        "String[] all = h.getAll(\"accept\");\n"
        "if (all.count() != 2) return -1;\n"
        "if (!all[0].equals(\"text/html\")) return -2;\n"
        "if (!all[1].equals(\"application/json\")) return -3;\n"
        "return 1;"), 1);
}

// fromUri derives the origin-form target (path + ?query) and a Host
// header (no port suffix for a default port).
TEST(HttpMessageTests, requestFromUriOriginFormAndHost) {
    EXPECT_EQ(runI32(
        "Uri u = Uri.parse(\"http://h.test/a/b?x=1&y=2\");\n"
        "HttpRequest r = HttpRequest.fromUri(\"GET\", u);\n"
        "if (!r.getTarget().equals(\"/a/b?x=1&y=2\")) return -1;\n"
        "String host = r.getHeaders().get(\"host\");\n"
        "if (host == null) return -2;\n"
        // default port 80 is implicit → no :port suffix.
        "if (!host.equals(\"h.test\")) return -3;\n"
        "return 1;"), 1);
}

// fromUri with an empty path normalizes the origin-form target to "/".
TEST(HttpMessageTests, requestFromUriEmptyPathBecomesSlash) {
    EXPECT_EQ(runI32(
        "Uri u = Uri.parse(\"http://h.test\");\n"
        "HttpRequest r = HttpRequest.fromUri(\"GET\", u);\n"
        "if (!r.getTarget().equals(\"/\")) return -1;\n"
        "return 1;"), 1);
}

// fromUri with an explicit non-default port carries the :port suffix
// in the Host header.
TEST(HttpMessageTests, requestFromUriExplicitPortInHost) {
    EXPECT_EQ(runI32(
        "Uri u = Uri.parse(\"http://h.test:8080/p\");\n"
        "HttpRequest r = HttpRequest.fromUri(\"GET\", u);\n"
        "String host = r.getHeaders().get(\"Host\");\n"
        "if (host == null) return -1;\n"
        "if (!host.equals(\"h.test:8080\")) return -2;\n"
        "if (!r.getTarget().equals(\"/p\")) return -3;\n"
        "return 1;"), 1);
}

// version() overrides the protocol version string.
TEST(HttpMessageTests, requestVersionOverride) {
    EXPECT_EQ(runI32(
        "HttpRequest r = HttpRequest.get(\"/\");\n"
        "r.version(\"HTTP/1.0\");\n"
        "if (!r.getVersion().equals(\"HTTP/1.0\")) return -1;\n"
        "return 1;"), 1);
}

// --- HttpResponse ------------------------------------------------------

// A default response is 200 OK HTTP/1.1, no body, classified 2xx.
TEST(HttpMessageTests, responseDefaults) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "if (r.statusCode() != 200) return -1;\n"
        "if (!r.getReason().equals(\"OK\")) return -2;\n"
        "if (!r.getVersion().equals(\"HTTP/1.1\")) return -3;\n"
        "if (!r.isSuccess()) return -4;\n"
        "if (r.isClientError()) return -5;\n"
        "if (r.hasBody) return -6;\n"
        "return 1;"), 1);
}

// of(status) fills the standard reason phrase for known codes.
TEST(HttpMessageTests, responseStandardReasonPhrases) {
    EXPECT_EQ(runI32(
        "HttpResponse a = HttpResponse.of(404);\n"
        "if (!a.getReason().equals(\"Not Found\")) return -1;\n"
        "HttpResponse b = HttpResponse.of(500);\n"
        "if (!b.getReason().equals(\"Internal Server Error\")) return -2;\n"
        "HttpResponse c = HttpResponse.of(301);\n"
        "if (!c.getReason().equals(\"Moved Permanently\")) return -3;\n"
        "HttpResponse d = HttpResponse.of(204);\n"
        "if (!d.getReason().equals(\"No Content\")) return -4;\n"
        "return 1;"), 1);
}

// An unrecognized code gets an empty reason phrase (advisory only).
TEST(HttpMessageTests, responseUnknownCodeEmptyReason) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.of(599);\n"
        "if (!r.getReason().equals(\"\")) return -1;\n"
        "if (!r.isServerError()) return -2;\n"
        "return 1;"), 1);
}

// The status classifiers cover the 1xx–5xx ranges.
TEST(HttpMessageTests, responseStatusClassifiers) {
    EXPECT_EQ(runI32(
        "HttpResponse i = HttpResponse.of(100);\n"
        "if (!i.isInformational()) return -1;\n"
        "HttpResponse s = HttpResponse.of(201);\n"
        "if (!s.isSuccess()) return -2;\n"
        "HttpResponse rd = HttpResponse.of(302);\n"
        "if (!rd.isRedirect()) return -3;\n"
        "HttpResponse ce = HttpResponse.of(403);\n"
        "if (!ce.isClientError()) return -4;\n"
        "HttpResponse se = HttpResponse.of(503);\n"
        "if (!se.isServerError()) return -5;\n"
        // a 2xx is none of informational/redirect/client/server.
        "if (s.isRedirect()) return -6;\n"
        "if (s.isClientError()) return -7;\n"
        "return 1;"), 1);
}

// reason() overrides the phrase; the numeric code is unchanged.
TEST(HttpMessageTests, responseReasonOverride) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.of(200);\n"
        "r.reason(\"All Good\");\n"
        "if (!r.getReason().equals(\"All Good\")) return -1;\n"
        "if (r.statusCode() != 200) return -2;\n"
        "return 1;"), 1);
}

// header() + body() on a response behave like the request side.
TEST(HttpMessageTests, responseHeadersAndBody) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.ok();\n"
        "r.setHeader(\"Content-Type\", \"text/plain\");\n"
        "int8[] payload = heap int8[2];\n"
        "payload[0] = (int8) 79;\n"    // 'O'
        "payload[1] = (int8) 75;\n"    // 'K'
        "r.body(payload, 2);\n"
        "if (!r.getHeaders().get(\"content-type\").equals(\"text/plain\")) return -1;\n"
        "if (!r.hasBody) return -2;\n"
        "if (r.bodyLength() != 2) return -3;\n"
        "if (r.body[0] != (int8) 79) return -4;\n"
        "return 1;"), 1);
}

// notFound() is the 404 convenience factory.
TEST(HttpMessageTests, responseNotFoundFactory) {
    EXPECT_EQ(runI32(
        "HttpResponse r = HttpResponse.notFound();\n"
        "if (r.statusCode() != 404) return -1;\n"
        "if (!r.getReason().equals(\"Not Found\")) return -2;\n"
        "if (!r.isClientError()) return -3;\n"
        "return 1;"), 1);
}
