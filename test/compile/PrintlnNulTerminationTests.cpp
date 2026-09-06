//
// `System.stdout.println(s)` must emit exactly the string's bytes and a
// newline — nothing more.
//
// It did not. The runtime's `println` takes a `const char*` and calls
// `strlen`, and the pointer comes from `__cajeta_string_cstr`, whose fast path
// handed out the raw buffer for a full-window string on the stated grounds
// that "builders guarantee a trailing NUL". Its test for that was
// `len == masked_count` — true exactly when the byte array holds precisely
// `len` bytes, i.e. when there is NO room for a terminator. And the guarantee
// was false: `StringBuilder.toString` does `allocBytes(len)` and returns
// `String(#out, len)`, so every string built that way was handed to `strlen`
// with no NUL after it, and the read ran on into the next heap block.
//
// The damage was not theoretical. The build tool's plugin protocol builds each
// NDJSON record with a StringBuilder and writes it with println, so records
// arrived as `{...}\x31` — one stray byte past the closing brace. The reader's
// validator rejected them and dropped them. A dropped `log` record was merely
// invisible; a dropped `output` record took the task's result with it
// (`cajeta cover` failing on `references undefined property 'cov.percent'`
// AFTER computing coverage correctly).
//
// Intermittent by nature — it depended on whether a zero byte happened to
// follow the allocation — which is why it survived as a puzzling one-off for
// as long as it did. These tests remove the luck: they assert the exact byte
// sequence, so a regression cannot hide behind a benign heap layout.
//

#include "gtest/gtest.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace {

namespace fs = std::filesystem;

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

fs::path freshTempDir() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_printnul_" + std::to_string(rng()));
    fs::create_directories(base / "src" / "demo");
    fs::create_directories(base / "build");
    return base;
}

// Captured with fread rather than fgets: the whole point is the byte AFTER the
// payload, and a line-oriented read would hide a stray byte that lands past a
// newline just as readily as it would show one before it.
std::string captureBytes(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen((cmd + " 2>NUL").c_str(), "rb");
#else
    FILE* p = popen((cmd + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str(), "r");
#endif
    if (!p) return out;
    std::array<char, 1024> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
#ifdef _WIN32
    _pclose(p);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
#else
    pclose(p);
#endif
    return out;
}

struct Built {
    fs::path base;
    fs::path bin;
    bool ok = false;
};

Built buildProgram(const std::string& body) {
    Built b;
    b.base = freshTempDir();
    {
        std::ofstream out(b.base / "src" / "demo" / "P.cajeta");
        out << "package demo;\n"
            << "public final class P {\n"
            << "    public static int32 main(String[] args) {\n"
            << body
            << "        return 0;\n"
            << "    }\n"
            << "}\n";
    }
    b.bin = b.base / "build" / "p";
    std::string cmd = compilerBinary() + " --emit=exe -o " + b.bin.string()
        + " demo.P.main " + (b.base / "src").string() + " "
        + (b.base / "build").string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
    bool built = std::system(cmd.c_str()) == 0;
    if (built && !fs::exists(b.bin)) {
        fs::path withExe = b.bin; withExe += ".exe";
        if (fs::exists(withExe)) b.bin = withExe;
    }
    b.ok = built && fs::exists(b.bin);
    return b;
}

bool haveCompiler() { return fs::exists(compilerBinary()); }

} // namespace

// ── the reported shape: a StringBuilder result ────────────────────────────
//
// The exact payload the plugin protocol emits, at the exact length that
// reproduced it. `strlen` on a non-terminated 48-byte buffer read one byte
// into the next allocation and printed it.
TEST(PrintlnNulTermination, AStringBuilderResultPrintsItsBytesAndNothingElse) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    const std::string payload =
        "{\"kind\":\"output\",\"key\":\"percent\",\"value\":\"36.0\"}";
    auto b = buildProgram(
        "        StringBuilder sb = stack StringBuilder();\n"
        "        sb.append(\"" + std::string(
            "{\\\"kind\\\":\\\"output\\\",\\\"key\\\":\\\"percent\\\","
            "\\\"value\\\":\\\"36.0\\\"}") + "\");\n"
        "        String s #= sb.toString();\n"
        "        System.stdout.println(s);\n");
    ASSERT_TRUE(b.ok) << "the probe program did not build";

    const std::string got = captureBytes(b.bin.string());
    EXPECT_EQ(got, payload + "\n")
        << "println emitted " << got.size() << " bytes for a "
        << payload.size() << "-byte string. A trailing byte here is `strlen` "
           "reading past a buffer that carries its length instead of a NUL.";
}

// The same property across a range of lengths. The defect depended on what
// happened to sit after the allocation, so one length proves little — a
// terminator that is absent will show up somewhere across a sweep even when a
// single case gets lucky and finds a zero byte.
TEST(PrintlnNulTermination, NoLengthLeaksATrailingByte) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto b = buildProgram(
        "        int32 n = 1;\n"
        "        while (n <= 80) {\n"
        "            StringBuilder sb = stack StringBuilder();\n"
        "            int32 i = 0;\n"
        "            while (i < n) { sb.append(\"x\"); i = i + 1; }\n"
        "            String s #= sb.toString();\n"
        "            System.stdout.println(s);\n"
        "            n = n + 1;\n"
        "        }\n");
    ASSERT_TRUE(b.ok) << "the probe program did not build";

    const std::string got = captureBytes(b.bin.string());
    std::string want;
    for (int n = 1; n <= 80; n++) want += std::string((size_t) n, 'x') + "\n";
    ASSERT_EQ(got.size(), want.size())
        << "total output is " << got.size() << " bytes, expected "
        << want.size() << " — some length leaked an extra byte";
    EXPECT_EQ(got, want);
}

// The other half: a string that DOES carry a terminator must still take the
// direct path and print correctly. Without this, "always copy into scratch"
// would satisfy the tests above while quietly discarding the fast path.
TEST(PrintlnNulTermination, ALiteralAndAConcatStillPrintExactly) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto b = buildProgram(
        "        System.stdout.println(\"literal\");\n"
        "        String t #= \"a\" + \"b\" + \"c\";\n"
        "        System.stdout.println(t);\n"
        "        String u #= \"prefix-\" + t;\n"
        "        System.stdout.println(u);\n");
    ASSERT_TRUE(b.ok) << "the probe program did not build";
    EXPECT_EQ(captureBytes(b.bin.string()), "literal\nabc\nprefix-abc\n");
}
