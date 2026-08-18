// Native-deps unit 15 — real HTTP Olla mirror fetch via CURL, against a
// localhost TestHttpServer. See native-deps-plan.md unit 15, spec §8.

#include "cajeta/buildtool/NativeProvision.h"
#include "cajeta/buildtool/ArtifactCache.h"
#include "TestHttpServer.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace cajeta::buildtool;
using cajeta::buildtool::testing::TestHttpServer;
namespace fs = std::filesystem;

namespace {
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
NativeLibrary redistLib(const std::string& id) {
    NativeLibrary l;
    l.id = id; l.version = "1.5.2"; l.license = "BSD-3-Clause";
    l.redistributable = true;
    return l;
}
} // namespace

// 15.1.1 — fetch a redistributable artifact over HTTP into the cache.
TEST(NativeOllaHttpTests, fetchesOverHttpIntoCache) {
    TestHttpServer srv;
    srv.route("/zstd/1.5.2/linux-x64/libzstd.a", 200, "ZSTD-HTTP-BYTES",
              "application/octet-stream");

    auto cacheRoot = (fs::temp_directory_path() / "nd-olla-http").string();
    fs::remove_all(cacheRoot);
    auto r = fetchFromOllaMirror(redistLib("zstd"), "1.5.2", "linux-x64",
                                 srv.baseUrl(), cacheRoot);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(*r, cacheRoot + "/zstd/1.5.2/linux-x64/libzstd.a");
    {
        // Scoped: Windows cannot remove_all below while this stream is open.
        std::ifstream f(*r, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), {});
        EXPECT_EQ(got, "ZSTD-HTTP-BYTES");
    }
    fs::remove_all(cacheRoot);
}

// 15.1.1 — sha256 verification holds over the HTTP path (mismatch fails loud).
TEST(NativeOllaHttpTests, httpShaMismatchFailsLoud) {
    TestHttpServer srv;
    srv.route("/zstd/1.5.2/linux-x64/libzstd.a", 200, "TAMPERED",
              "application/octet-stream");
    auto cacheRoot = (fs::temp_directory_path() / "nd-olla-http-sha").string();
    fs::remove_all(cacheRoot);
    auto lib = redistLib("zstd");
    NativeArtifact a; a.platform = "linux-x64"; a.sha256 = "sha256:deadbeef";
    lib.artifacts["linux-x64"] = a;
    auto r = fetchFromOllaMirror(lib, "1.5.2", "linux-x64", srv.baseUrl(),
                                 cacheRoot);
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("sha256 mismatch"), std::string::npos);
    fs::remove_all(cacheRoot);
}

// A 404 on the mirror fails loud.
TEST(NativeOllaHttpTests, http404FailsLoud) {
    TestHttpServer srv;   // no route registered → 404
    auto cacheRoot = (fs::temp_directory_path() / "nd-olla-http-404").string();
    auto r = fetchFromOllaMirror(redistLib("zstd"), "1.5.2", "linux-x64",
                                 srv.baseUrl(), cacheRoot);
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("native http"), std::string::npos);
    fs::remove_all(cacheRoot);
}

// Embargoed stays metadata-only even with an HTTP mirror (guard precedes fetch).
TEST(NativeOllaHttpTests, embargoedNotFetchedOverHttp) {
    TestHttpServer srv;
    srv.route("/vx/2.0/linux-x64/libvx.a", 200, "X", "application/octet-stream");
    auto lib = redistLib("vx");
    lib.redistributable = false;
    auto r = fetchFromOllaMirror(lib, "2.0", "linux-x64", srv.baseUrl(),
                                 (fs::temp_directory_path() / "nd-x").string());
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("embargoed"), std::string::npos);
}
