// End-to-end tests for --emit=archive / --emit=uber. Drives the
// in-tree cajeta compiler via fork+exec, points it at a tiny
// cajeta source tree, then inspects the resulting .cja file for
// the expected header / manifest / entry shape.
//
// The bitcode-roundtrip side (parsing the embedded bitcode and
// validating it) lands when CajetaArchive grows a reader (which is
// the prerequisite for --classpath ingestion and full --emit=uber).
// For v1 these tests pin "the writer in --emit=archive mode produces
// the same bytes the unit-level CajetaArchiveTests verify the
// underlying writer produces."

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
}

uint32_t readU32LE(const std::vector<uint8_t>& bytes, size_t offset) {
    return  (uint32_t) bytes[offset]
        | (((uint32_t) bytes[offset + 1]) << 8)
        | (((uint32_t) bytes[offset + 2]) << 16)
        | (((uint32_t) bytes[offset + 3]) << 24);
}

uint64_t readU64LE(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= ((uint64_t) bytes[offset + (size_t) i]) << (i * 8);
    }
    return v;
}

// Resolve the in-tree compiler binary from CAJETA_SOURCE_ROOT (set by
// the test runner). Falls back to the default build path when the env
// var isn't set.
std::string compilerPath() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root) root = ".";
    return std::string(root) + "/build/src/cajeta";
}

// Lay out a one-file cajeta source tree under a fresh temp dir,
// returning (sourceRoot, buildRoot). Caller cleans up. Body is a
// minimal main that exits 0.
struct TmpProject {
    fs::path sourceRoot;
    fs::path buildRoot;
};

TmpProject makeTmpProject(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_archive_" + tag + "_" + std::to_string(rng()));
    auto src = base / "src" / "demo";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    std::ofstream out(src / "Hello.cajeta");
    out << "package demo;\n"
        << "public final class Hello {\n"
        << "    public static int32 run() { return 0; }\n"
        << "}\n";
    return TmpProject{base / "src", build};
}

} // namespace

TEST(CajetaArchiveEmitTests, archiveModeWritesCjaWithMagicHeader) {
    auto proj = makeTmpProject("magic");
    std::string cmd =
        compilerPath()
        + " --emit=archive demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "compiler exited non-zero: " << cmd;

    auto cja = proj.buildRoot / "Hello.cja";
    ASSERT_TRUE(fs::exists(cja)) << "expected " << cja;
    auto bytes = readBytes(cja.string());
    ASSERT_GE(bytes.size(), (size_t) 40);

    EXPECT_EQ(std::string((const char*) bytes.data(), 8), std::string("CAJETA01"));
    EXPECT_EQ(readU32LE(bytes, 8),  1u);      // format version
    EXPECT_EQ(readU32LE(bytes, 12), 0u);      // flags (v1: no compression)
    EXPECT_EQ(readU64LE(bytes, 16), 0u);      // index offset (v1)
    EXPECT_EQ(readU64LE(bytes, 24), 0u);      // index length (v1)

    fs::remove_all(proj.sourceRoot.parent_path());
}

TEST(CajetaArchiveEmitTests, archiveModeManifestSaysThin) {
    auto proj = makeTmpProject("thin");
    std::string cmd =
        compilerPath()
        + " --emit=archive demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto bytes = readBytes((proj.buildRoot / "Hello.cja").string());
    uint64_t manifestLen = readU64LE(bytes, 32);
    std::string manifest((const char*) bytes.data() + 40, manifestLen);
    EXPECT_NE(manifest.find("\"kind\":\"thin\""), std::string::npos)
        << "manifest = " << manifest;
    EXPECT_NE(manifest.find("\"name\":\"demo.Hello\""), std::string::npos);

    fs::remove_all(proj.sourceRoot.parent_path());
}

TEST(CajetaArchiveEmitTests, uberModeManifestSaysUber) {
    auto proj = makeTmpProject("uber");
    std::string cmd =
        compilerPath()
        + " --emit=uber demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto bytes = readBytes((proj.buildRoot / "Hello.cja").string());
    uint64_t manifestLen = readU64LE(bytes, 32);
    std::string manifest((const char*) bytes.data() + 40, manifestLen);
    EXPECT_NE(manifest.find("\"kind\":\"uber\""), std::string::npos)
        << "manifest = " << manifest;

    fs::remove_all(proj.sourceRoot.parent_path());
}

TEST(CajetaArchiveEmitTests, archiveContainsUserAndStdlibEntries) {
    auto proj = makeTmpProject("entries");
    std::string cmd =
        compilerPath()
        + " --emit=archive demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto bytes = readBytes((proj.buildRoot / "Hello.cja").string());
    uint64_t manifestLen = readU64LE(bytes, 32);
    std::string manifest((const char*) bytes.data() + 40, manifestLen);

    // We expect at least two entries: the user's demo.Hello and the
    // stdlib bundle (cajeta.runtime.__stdlib__). entry_count's exact
    // value depends on how many modules the compiler retains today;
    // pin a lower bound rather than exact for forward-compatibility.
    auto countPos = manifest.find("\"entry_count\":");
    ASSERT_NE(countPos, std::string::npos);
    int count = std::atoi(manifest.c_str() + countPos
        + std::strlen("\"entry_count\":"));
    EXPECT_GE(count, 2);

    // First entry is a user (origin tag 0) or stdlib (1) bitcode file.
    // Walk past header (32) + manifest_len (8) + manifest (manifestLen).
    size_t cursor = 32 + 8 + manifestLen;
    ASSERT_LT(cursor + 4, bytes.size());
    uint32_t nameLen = readU32LE(bytes, cursor);
    cursor += 4;
    ASSERT_LE(cursor + nameLen, bytes.size());
    std::string firstName((const char*) bytes.data() + cursor, nameLen);
    // Name should be a .bc with '/' separators (jar-style convention).
    EXPECT_NE(firstName.find(".bc"), std::string::npos)
        << "first entry name = " << firstName;

    fs::remove_all(proj.sourceRoot.parent_path());
}
