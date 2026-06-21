// Native-deps unit 11 — actionable fail-loud diagnostics + the
// provision-time-only network seam. See native-deps-plan.md unit 11, spec §9.

#include "cajeta/buildtool/NativeProvision.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>
#include <vector>

using namespace cajeta::buildtool;

namespace {
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

// 11.1.1 — missing message names lib/version/platform, probed dirs, and the fix.
TEST(NativeDiagnosticsTests, missingMessageIsActionable) {
    std::string m = nativeMissingMessage(
        "zstd", "1.5.2", "linux-arm64",
        {"./native", "~/.cajeta/native"}, "");
    EXPECT_TRUE(has(m, "zstd"));
    EXPECT_TRUE(has(m, "1.5.2"));
    EXPECT_TRUE(has(m, "linux-arm64"));
    EXPECT_TRUE(has(m, "./native"));
    EXPECT_TRUE(has(m, "cajeta fetch"));
    EXPECT_TRUE(has(m, "CAJETA_NATIVE_PATH"));
}

// 11.1.1 — an embargoed lib's acquire note is surfaced in the missing message.
TEST(NativeDiagnosticsTests, missingMessageIncludesAcquire) {
    std::string m = nativeMissingMessage(
        "vendorx", "2.0", "linux-x64", {"./native"},
        "download from vendor.example/dl");
    EXPECT_TRUE(has(m, "vendor.example/dl"));
}

// 11.1.2 — checksum mismatch and unsupported platform are distinct, named.
TEST(NativeDiagnosticsTests, distinctNamedFailures) {
    std::string cm = nativeChecksumMismatchMessage("zstd", "sha256:aa", "sha256:bb");
    EXPECT_TRUE(has(cm, "sha256 mismatch"));
    EXPECT_TRUE(has(cm, "sha256:aa"));
    EXPECT_TRUE(has(cm, "sha256:bb"));

    std::string up = nativeUnsupportedPlatformMessage(
        "zstd", "freebsd-x64", {"linux-x64", "macos-arm64"});
    EXPECT_TRUE(has(up, "freebsd-x64"));
    EXPECT_TRUE(has(up, "not available"));
    EXPECT_TRUE(has(up, "linux-x64"));
}

// 11.1.3 / 11.2.2 — the network seam: Execution phase hard-fails a net op.
TEST(NativeDiagnosticsTests, networkSeamGuardsExecutionPhase) {
    EXPECT_EQ(nativePhase(), NativePhase::Provision);   // default

    // Provision → allowed.
    auto ok = guardNativeNetwork("fetch");
    EXPECT_FALSE((bool) ok);
    consumeError(std::move(ok));

    // Execution → hard error.
    setNativePhase(NativePhase::Execution);
    auto blocked = guardNativeNetwork("fetch");
    ASSERT_TRUE((bool) blocked);
    EXPECT_NE(errText(std::move(blocked)).find("execution/JIT time"),
              std::string::npos);
    setNativePhase(NativePhase::Provision);             // reset for other suites
}

// 11.2.2 — a mirror fetch attempted at execution time is refused before the net.
TEST(NativeDiagnosticsTests, ollaFetchRefusedAtExecutionTime) {
    NativeLibrary lib;
    lib.id = "zstd"; lib.version = "1.5.2"; lib.license = "BSD-3-Clause";
    lib.redistributable = true;

    setNativePhase(NativePhase::Execution);
    auto r = fetchFromOllaMirror(lib, "1.5.2", "linux-x64",
                                 "/tmp/nd-mirror", "/tmp/nd-cache");
    setNativePhase(NativePhase::Provision);             // reset first
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("execution/JIT time"),
              std::string::npos);
}
