// A macOS release must link a user program's OpenSSL statically from the
// install, so the program runs on a Mac without Homebrew (Julian, 2026-10-27,
// cajeta-cloud-adapters 5.3.3). v0.46.3 through v0.47.0 passed
// `-L<the build machine's Cellar>/openssl@4/4.0.3/lib -lssl -lcrypto`. These
// pin the pure helper the link line stands on; they run on every host.

#include "cajeta/compile/MacHostPaths.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace cajeta;

TEST(MacHostPathsTests, bundledStaticOpenSslIsLinkedByPath) {
    std::set<std::string> present = {"/opt/cajeta/bin/cajeta-deps/libssl.a",
                                     "/opt/cajeta/bin/cajeta-deps/libcrypto.a"};
    auto exists = [&](const std::string& p) { return present.count(p) > 0; };
    std::vector<std::string> want = {"/opt/cajeta/bin/cajeta-deps/libssl.a",
                                     "/opt/cajeta/bin/cajeta-deps/libcrypto.a"};
    EXPECT_EQ(macOpenSslLinkArgs("/opt/cajeta/bin", "/opt/homebrew/Cellar/openssl@4/4.0.3/lib",
                                 exists),
              want);
}

// Does not fire: a development build has no bundled archives and keeps the
// configured directory.
TEST(MacHostPathsTests, withoutBundledArchivesTheConfiguredDirectoryIsSearched) {
    auto none = [](const std::string&) { return false; };
    std::vector<std::string> want = {"-L/opt/homebrew/opt/openssl@4/lib", "-lssl", "-lcrypto"};
    EXPECT_EQ(macOpenSslLinkArgs("/x/build/src", "/opt/homebrew/opt/openssl@4/lib", none), want);
    std::vector<std::string> bare = {"-lssl", "-lcrypto"};
    EXPECT_EQ(macOpenSslLinkArgs("/x/build/src", "", none), bare);
}

// Both archives or neither: libssl.a alone would leave libcrypto to a search
// path the program's Mac may not have.
TEST(MacHostPathsTests, oneBundledArchiveAloneIsNotUsed) {
    std::set<std::string> present = {"/opt/cajeta/bin/cajeta-deps/libssl.a"};
    auto exists = [&](const std::string& p) { return present.count(p) > 0; };
    std::vector<std::string> want = {"-L/d", "-lssl", "-lcrypto"};
    EXPECT_EQ(macOpenSslLinkArgs("/opt/cajeta/bin", "/d", exists), want);
}
