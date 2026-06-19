// NET-9.3 — cajeta.io.net.http.Router tests (minimal HTTP router, path params).
//
// The router is PURE LOGIC over the HttpRequest / HttpResponse data types
// (NET-7.1) — no socket, no native — so it tests directly over the JIT with
// golden vectors, exactly like HttpServerTests' pure byte path. Each test
// compiles a small Cajeta run() that builds a Router, registers routes with
// lambda handlers, builds an HttpRequest (method + target), calls
// router.dispatch(req), and returns an int32 sentinel (1 on success, a
// distinct negative per failed sub-check).
//
// A handler reads a captured path param via req.pathParam("id"); a route
// that registered a `{id}` segment binds the (percent-decoded) request
// segment before its handler runs.
//
// Pins NET-9.3: "Minimal router: method + path-pattern matching
// (`/users/{id}` path params), 404/405 defaults, handler registration.
// Deliberately minimal — not a web framework." (plan/cajeta-net-plan.md,
// Phase 9) and the acceptance row
// `HttpServerTests.routerPathParamsAnd404405` (router dispatches
// `/users/{id}` extracting the path param; unmatched path -> 404, wrong
// method -> 405).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the router, the message types,
// and String. The body must `return` int32. A `req(method, target)` helper
// builds a bare HttpRequest; routes are registered on a local `r`.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.http.Router;\n"
        "import cajeta.io.net.http.HttpRequest;\n"
        "import cajeta.io.net.http.HttpResponse;\n"
        "public final class M {\n"
        "    // A bare request with an explicit method + target.\n"
        "    static #HttpRequest req(String method, String target) {\n"
        "        return HttpRequest.of(method, target);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- a literal route dispatches to its handler --------------------------

TEST(HttpRouterTests, literalRouteDispatches) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/health\", (req) -> HttpResponse.of(204));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/health\"));\n"
        "if (resp.statusCode() != 204) return -1;\n"
        "return 1;"), 1);
}

// --- the headline: /users/{id} extracts the path param ------------------

// The acceptance shape: GET /users/42 matches /users/{id}, the handler
// reads id == "42", and answers 200.
TEST(HttpRouterTests, pathParamIsExtractedAndReadable) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    String id = req.pathParam(\"id\");\n"
        "    if (id == null) { return HttpResponse.of(500); }\n"
        "    if (!id.equals(\"42\")) { return HttpResponse.of(400); }\n"
        "    return HttpResponse.of(200);\n"
        "};\n"
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", h);\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/42\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// Two params in one pattern both bind, in order.
TEST(HttpRouterTests, twoPathParamsBind) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    String org = req.pathParam(\"org\");\n"
        "    String repo = req.pathParam(\"repo\");\n"
        "    if (org == null || repo == null) { return HttpResponse.of(500); }\n"
        "    if (!org.equals(\"acme\")) { return HttpResponse.of(401); }\n"
        "    if (!repo.equals(\"cajeta\")) { return HttpResponse.of(402); }\n"
        "    return HttpResponse.of(200);\n"
        "};\n"
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/repos/{org}/{repo}\", h);\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/repos/acme/cajeta\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// A param value is percent-DECODED before binding (a %2F in a segment).
TEST(HttpRouterTests, pathParamIsPercentDecoded) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    String id = req.pathParam(\"id\");\n"
        "    if (id == null) { return HttpResponse.of(500); }\n"
        // %61%62 decodes to "ab".
        "    if (!id.equals(\"ab\")) { return HttpResponse.of(400); }\n"
        "    return HttpResponse.of(200);\n"
        "};\n"
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/u/{id}\", h);\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/u/%61%62\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// The query string is stripped before segment matching: /users/42?x=1
// still matches /users/{id} and binds id == "42" (no "?x=1").
TEST(HttpRouterTests, queryStringIsStrippedFromPathMatch) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    String id = req.pathParam(\"id\");\n"
        "    if (id == null) { return HttpResponse.of(500); }\n"
        "    if (!id.equals(\"42\")) { return HttpResponse.of(400); }\n"
        "    return HttpResponse.of(200);\n"
        "};\n"
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", h);\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/42?x=1&y=2\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// --- 404: no route's path matches ---------------------------------------

TEST(HttpRouterTests, unmatchedPathIs404) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/widgets/7\"));\n"
        "if (resp.statusCode() != 404) return -1;\n"
        "return 1;"), 1);
}

// A different segment COUNT does not match (no catch-all): /users/{id}
// (2 segs) does not match /users/42/extra (3 segs).
TEST(HttpRouterTests, segmentCountMustMatch) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/42/extra\"));\n"
        "if (resp.statusCode() != 404) return -1;\n"
        "return 1;"), 1);
}

// An empty router 404s everything.
TEST(HttpRouterTests, emptyRouter404s) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/anything\"));\n"
        "if (resp.statusCode() != 404) return -1;\n"
        "if (r.size() != 0) return -2;\n"
        "return 1;"), 1);
}

// --- 405: path matches but method does not ------------------------------

TEST(HttpRouterTests, wrongMethodIs405) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"POST\", \"/users/42\"));\n"
        "if (resp.statusCode() != 405) return -1;\n"
        "return 1;"), 1);
}

// 405 carries an Allow header listing the registered methods for the path.
TEST(HttpRouterTests, methodNotAllowedListsAllowHeader) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "r.route(\"PUT\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"DELETE\", \"/users/42\"));\n"
        "if (resp.statusCode() != 405) return -1;\n"
        "String allow = resp.getHeaders().get(\"Allow\");\n"
        "if (allow == null) return -2;\n"
        // Both registered methods are present (order: registration order).
        "if (!allow.equals(\"GET, PUT\")) return -3;\n"
        "return 1;"), 1);
}

// --- method correctness: same path, different methods route separately --

TEST(HttpRouterTests, samePathDifferentMethodsDispatchSeparately) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/item\", (req) -> HttpResponse.of(200));\n"
        "r.route(\"POST\", \"/item\", (req) -> HttpResponse.of(201));\n"
        "HttpResponse g = r.dispatch(M.req(\"GET\", \"/item\"));\n"
        "if (g.statusCode() != 200) return -1;\n"
        "HttpResponse p = r.dispatch(M.req(\"POST\", \"/item\"));\n"
        "if (p.statusCode() != 201) return -2;\n"
        "return 1;"), 1);
}

// Method matching is case-sensitive (HTTP methods are case-sensitive):
// a "get" request does not match a "GET" route -> 405 (path matched).
TEST(HttpRouterTests, methodMatchIsCaseSensitive) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/x\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"get\", \"/x\"));\n"
        "if (resp.statusCode() != 405) return -1;\n"
        "return 1;"), 1);
}

// --- registration order: first matching route wins ----------------------

TEST(HttpRouterTests, firstMatchingRouteWins) {
    EXPECT_EQ(runI32(
        // A specific-looking literal-vs-param: register the param route
        // first; it wins for /users/me even though a later literal also
        // matches (first-match dispatch).
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "r.route(\"GET\", \"/users/me\", (req) -> HttpResponse.of(222));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/me\"));\n"
        // The first registration (the param route) wins -> 200, not 222.
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}

// A literal route registered FIRST shadows the param route after it.
TEST(HttpRouterTests, literalBeforeParamWins) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/me\", (req) -> HttpResponse.of(222));\n"
        "r.route(\"GET\", \"/users/{id}\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/me\"));\n"
        "if (resp.statusCode() != 222) return -1;\n"
        // And a different id still falls through to the param route.
        "HttpResponse other = r.dispatch(M.req(\"GET\", \"/users/7\"));\n"
        "if (other.statusCode() != 200) return -2;\n"
        "return 1;"), 1);
}

// --- the root route ("/") matches the empty path ------------------------

TEST(HttpRouterTests, rootRouteMatchesSlash) {
    EXPECT_EQ(runI32(
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/\", (req) -> HttpResponse.of(200));\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        // A non-root path does not match the root route.
        "HttpResponse other = r.dispatch(M.req(\"GET\", \"/x\"));\n"
        "if (other.statusCode() != 404) return -2;\n"
        "return 1;"), 1);
}

// --- pathParam on a request that never went through a router is null ----

TEST(HttpRouterTests, pathParamNullWithoutRouter) {
    EXPECT_EQ(runI32(
        "HttpRequest req = M.req(\"GET\", \"/users/42\");\n"
        "if (req.pathParam(\"id\") != null) return -1;\n"
        "return 1;"), 1);
}

// A trailing slash on the request path is normalized away (so /users/42/
// still matches /users/{id}).
TEST(HttpRouterTests, trailingSlashNormalized) {
    EXPECT_EQ(runI32(
        "(HttpRequest) -> #HttpResponse h = (req) -> {\n"
        "    String id = req.pathParam(\"id\");\n"
        "    if (id == null) { return HttpResponse.of(500); }\n"
        "    if (!id.equals(\"42\")) { return HttpResponse.of(400); }\n"
        "    return HttpResponse.of(200);\n"
        "};\n"
        "Router r = heap Router();\n"
        "r.route(\"GET\", \"/users/{id}\", h);\n"
        "HttpResponse resp = r.dispatch(M.req(\"GET\", \"/users/42/\"));\n"
        "if (resp.statusCode() != 200) return -1;\n"
        "return 1;"), 1);
}
