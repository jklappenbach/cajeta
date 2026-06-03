// Regression tests for the Phase 6b HTTP repository driver. These
// guard:
//   - settings.repositories[].auth parsing for bearer + mtls forms
//     (including the documented error paths).
//   - The driver's wire conformance to the v1 REST API in
//     BuildTool.md "HTTP repository" — listVersions, fetch, and
//     fetchManifestJson endpoints.
//   - Bearer-token authentication: the configured token reaches the
//     server's Authorization header verbatim, both literal and
//     env-var forms.
//   - The 404-falls-through contracts: an unknown package returns
//     an empty version list (not an error) and a missing manifest
//     sidecar returns nullopt so the MVS walker treats the dep as a
//     leaf — matching the filesystem driver's behaviour.
//
// Live HTTP tests use a tiny in-process socket server (single
// connection at a time, blocking accept loop on a background
// thread). It speaks just enough HTTP/1.1 to round-trip GETs with
// canned responses — no chunked encoding, no keep-alive games. The
// whole server is under 150 LOC of POSIX socket code, kept local
// to this test file so the production source has no test-only
// surface.

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/repo/HttpRepository.h"
#include "TestHttpServer.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using cajeta::buildtool::HttpRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseRepositories;
using cajeta::buildtool::RepositoryAuth;
using cajeta::buildtool::RepositorySpec;
using cajeta::buildtool::testing::TestHttpServer;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-http-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }


} // namespace

// ─── settings.repositories[].auth parsing ─────────────────────────────

TEST(HttpRepositoryTests, parsesBearerAuthFromManifest) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "nexus", "url": "https://nexus.local",
                  "auth": { "type": "bearer", "token-env": "NEXUS_TOKEN" } }
            ]
        }
    })");
    auto specs = parseRepositories(m);
    ASSERT_TRUE((bool)specs) << errorText(specs.takeError());
    ASSERT_EQ(specs->size(), 1u);
    EXPECT_EQ((*specs)[0].auth.type, "bearer");
    EXPECT_EQ((*specs)[0].auth.tokenEnv, "NEXUS_TOKEN");
}

TEST(HttpRepositoryTests, parsesMtlsAuthFromManifest) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "secure", "url": "https://secure.local",
                  "auth": {
                      "type": "mtls",
                      "client-cert": "/etc/ssl/cajeta-client.pem",
                      "client-key":  "/etc/ssl/cajeta-client.key",
                      "ca-cert":     "/etc/ssl/cajeta-ca.pem"
                  } }
            ]
        }
    })");
    auto specs = parseRepositories(m);
    ASSERT_TRUE((bool)specs) << errorText(specs.takeError());
    EXPECT_EQ((*specs)[0].auth.type, "mtls");
    EXPECT_EQ((*specs)[0].auth.clientCertPath,
              "/etc/ssl/cajeta-client.pem");
    EXPECT_EQ((*specs)[0].auth.clientKeyPath,
              "/etc/ssl/cajeta-client.key");
    EXPECT_EQ((*specs)[0].auth.caCertPath, "/etc/ssl/cajeta-ca.pem");
}

TEST(HttpRepositoryTests, errorsOnBearerWithoutTokenSource) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "broken", "url": "https://x.y",
                  "auth": { "type": "bearer" } }
            ]
        }
    })");
    auto specs = parseRepositories(m);
    ASSERT_FALSE((bool)specs);
    auto msg = errorText(specs.takeError());
    EXPECT_NE(msg.find("bearer"), std::string::npos);
    EXPECT_NE(msg.find("token"), std::string::npos);
}

TEST(HttpRepositoryTests, errorsOnMtlsWithoutCertOrKey) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "broken", "url": "https://x.y",
                  "auth": { "type": "mtls",
                            "client-cert": "/etc/cert.pem" } }
            ]
        }
    })");
    auto specs = parseRepositories(m);
    ASSERT_FALSE((bool)specs);
    auto msg = errorText(specs.takeError());
    EXPECT_NE(msg.find("mtls"), std::string::npos);
    EXPECT_NE(msg.find("client-key"), std::string::npos);
}

TEST(HttpRepositoryTests, errorsOnUnknownAuthType) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "broken", "url": "https://x.y",
                  "auth": { "type": "magic" } }
            ]
        }
    })");
    auto specs = parseRepositories(m);
    ASSERT_FALSE((bool)specs);
    auto msg = errorText(specs.takeError());
    EXPECT_NE(msg.find("auth.type"), std::string::npos);
}

// ─── live HTTP wire conformance ───────────────────────────────────────

TEST(HttpRepositoryTests, listVersionsParsesV1Response) {
    TestHttpServer srv;
    srv.route("/cajeta.io.net.http/versions.json", 200,
              R"({"versions":["1.0.0","1.2.3","1.2.4"],"deprecated":[]})");

    auto stage = makeTempDir("listvers");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto vers = repo.listVersions("cajeta.io.net.http");
    ASSERT_TRUE((bool)vers) << errorText(vers.takeError());
    ASSERT_EQ(vers->size(), 3u);
    EXPECT_EQ((*vers)[0], "1.0.0");
    EXPECT_EQ((*vers)[1], "1.2.3");
    EXPECT_EQ((*vers)[2], "1.2.4");

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, listVersionsReturnsEmptyOn404) {
    TestHttpServer srv;
    // No route registered → 404.

    auto stage = makeTempDir("listmiss");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto vers = repo.listVersions("does.not.exist");
    ASSERT_TRUE((bool)vers) << errorText(vers.takeError());
    EXPECT_TRUE(vers->empty());

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, fetchWritesArtifactToStageDir) {
    TestHttpServer srv;
    const std::string body = "fake .cja bytes";
    srv.route("/some.pkg/1.0.0/some.pkg-1.0.0.cja", 200, body);

    auto stage = makeTempDir("fetch");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto path = repo.fetch("some.pkg", "1.0.0");
    ASSERT_TRUE((bool)path) << errorText(path.takeError());
    ASSERT_TRUE(std::filesystem::exists(*path));
    std::ifstream in(*path, std::ios::binary);
    std::ostringstream got; got << in.rdbuf();
    EXPECT_EQ(got.str(), body);

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, fetchManifestJsonReturnsManifestEndpoint) {
    TestHttpServer srv;
    const std::string manifest =
        R"({"details":{"name":"some.pkg","version":"1.0.0"}})";
    srv.route("/some.pkg/1.0.0/manifest.json", 200, manifest);

    auto stage = makeTempDir("manifest");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto js = repo.fetchManifestJson("some.pkg", "1.0.0");
    ASSERT_TRUE((bool)js) << errorText(js.takeError());
    ASSERT_TRUE(js->has_value());
    EXPECT_EQ(**js, manifest);

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, fetchManifestJsonReturnsNulloptOn404) {
    TestHttpServer srv;
    // No manifest endpoint registered → 404 → backwards-compat
    // path (treated as a leaf by the walker).
    auto stage = makeTempDir("manifest-miss");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto js = repo.fetchManifestJson("pre.sidecar", "0.1.0");
    ASSERT_TRUE((bool)js) << errorText(js.takeError());
    EXPECT_FALSE(js->has_value());

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, fetchErrorsOnNonSuccessStatus) {
    TestHttpServer srv;
    srv.route("/p/1.0.0/p-1.0.0.cja", 500, "internal error");

    auto stage = makeTempDir("fetch-err");
    HttpRepository repo("test", srv.baseUrl(),
                        RepositoryAuth{}, stage.string());
    auto path = repo.fetch("p", "1.0.0");
    ASSERT_FALSE((bool)path);
    auto msg = errorText(path.takeError());
    EXPECT_NE(msg.find("500"), std::string::npos)
        << "expected status citation, got: " << msg;

    std::filesystem::remove_all(stage);
}

// ─── bearer-token auth (literal and env-var forms) ────────────────────

TEST(HttpRepositoryTests, bearerLiteralTokenLandsInAuthHeader) {
    TestHttpServer srv;
    srv.route("/p/versions.json", 200, R"({"versions":["1.0.0"]})");

    RepositoryAuth auth;
    auth.type = "bearer";
    auth.tokenLiteral = "secret-literal-xyz";

    auto stage = makeTempDir("bearer-lit");
    HttpRepository repo("test", srv.baseUrl(), auth, stage.string());
    auto vers = repo.listVersions("p");
    ASSERT_TRUE((bool)vers) << errorText(vers.takeError());

    EXPECT_EQ(srv.lastAuthHeader(), "Bearer secret-literal-xyz");

    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, bearerEnvTokenLandsInAuthHeader) {
    TestHttpServer srv;
    srv.route("/p/versions.json", 200, R"({"versions":["1.0.0"]})");

    ::setenv("CAJETA_HTTP_TESTS_TOKEN",
             "secret-env-abc", /*overwrite=*/1);

    RepositoryAuth auth;
    auth.type = "bearer";
    auth.tokenEnv = "CAJETA_HTTP_TESTS_TOKEN";

    auto stage = makeTempDir("bearer-env");
    HttpRepository repo("test", srv.baseUrl(), auth, stage.string());
    auto vers = repo.listVersions("p");
    ASSERT_TRUE((bool)vers) << errorText(vers.takeError());

    EXPECT_EQ(srv.lastAuthHeader(), "Bearer secret-env-abc");

    ::unsetenv("CAJETA_HTTP_TESTS_TOKEN");
    std::filesystem::remove_all(stage);
}

TEST(HttpRepositoryTests, missingEnvVarSendsNoAuthHeader) {
    // Configured to read $UNSET_TOKEN_THAT_DOES_NOT_EXIST. The driver
    // must not synthesize a malformed header — the request goes out
    // with no Authorization, and the server (which has no auth check)
    // happily serves the resource.
    TestHttpServer srv;
    srv.route("/p/versions.json", 200, R"({"versions":["1.0.0"]})");

    RepositoryAuth auth;
    auth.type = "bearer";
    auth.tokenEnv = "UNSET_TOKEN_THAT_DOES_NOT_EXIST_84621";

    auto stage = makeTempDir("bearer-missing");
    HttpRepository repo("test", srv.baseUrl(), auth, stage.string());
    auto vers = repo.listVersions("p");
    ASSERT_TRUE((bool)vers) << errorText(vers.takeError());
    EXPECT_EQ(srv.lastAuthHeader(), "");

    std::filesystem::remove_all(stage);
}
