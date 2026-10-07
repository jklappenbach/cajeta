// diagnostic-location Unit 1 (spec 2, 5.1): every diagnostic names the kind of place it points at
// in `at`, the legacy file/line/column are unchanged, and every field it writes is in the schema.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include <llvm/Support/JSON.h>

#include "cajeta/error/Diagnostics.h"

namespace fs = std::filesystem;

namespace {

fs::path sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string emitted(const std::string& file, int line, int column, const std::string& origin) {
    testing::internal::CaptureStderr();
    cajeta::emitJsonDiagnostic("error", "CAJETA_ERROR_X", "m", file, line, column, origin);
    return testing::internal::GetCapturedStderr();
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(DiagnosticLocation, aProjectFileIsSourceAndKeepsItsLegacyFields) {
    std::string out = emitted("test/Keep.cajeta", 9, 31, "project");
    EXPECT_TRUE(has(out, "\"file\":\"test/Keep.cajeta\",\"line\":9,\"column\":31,\"origin\":\"project\"")) << out;
    EXPECT_TRUE(has(out, "\"at\":\"source\"")) << out;
    EXPECT_FALSE(has(out, "\"archive\"")) << out;
}

TEST(DiagnosticLocation, aStdlibFileIsStdlib) {
    std::string out = emitted("<stdlib>/cajeta/hash/Sha256.cajeta", 40, 5, "stdlib");
    EXPECT_TRUE(has(out, "\"file\":\"<stdlib>/cajeta/hash/Sha256.cajeta\"")) << out;
    EXPECT_TRUE(has(out, "\"at\":\"stdlib\"")) << out;
}

TEST(DiagnosticLocation, anArchiveEntryIsArchiveAndNamesTheArchive) {
    std::string out = emitted("dev.cajeta.unit-0.3.3.cja!dev/cajeta/unit/Runner.cajeta", 61, 9,
                              "dependency");
    EXPECT_TRUE(has(out, "\"file\":\"dev.cajeta.unit-0.3.3.cja!dev/cajeta/unit/Runner.cajeta\"")) << out;
    EXPECT_TRUE(has(out, "\"at\":\"archive\"")) << out;
    EXPECT_TRUE(has(out, "\"archive\":\"dev.cajeta.unit-0.3.3.cja\"")) << out;
}

TEST(DiagnosticLocation, anUnlocatedDiagnosticHasNoKind) {
    std::string out = emitted("", -1, -1, "project");
    EXPECT_TRUE(has(out, "\"file\":null,\"line\":null,\"column\":null")) << out;
    EXPECT_TRUE(has(out, "\"at\":null")) << out;
}

// Every key a diagnostic record carries is documented in the schema's diagnostic definition.
TEST(DiagnosticLocation, everyFieldADiagnosticWritesIsInTheSchema) {
    std::ifstream in(sourceRoot() / "specs" / "schemas" / "compiler-jsonl.schema.json");
    std::string schemaText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto schema = llvm::json::parse(schemaText);
    ASSERT_TRUE((bool) schema) << llvm::toString(schema.takeError());
    const auto* props = schema->getAsObject()->getObject("$defs")->getObject("diagnostic")
                            ->getObject("properties");
    ASSERT_NE(props, nullptr);
    for (const std::string& file : {std::string("test/Keep.cajeta"),
                                    std::string("a.cja!x/Y.cajeta"), std::string("")}) {
        std::string out = emitted(file, 3, 4, "project");
        auto record = llvm::json::parse(out.substr(0, out.find('\n')));
        ASSERT_TRUE((bool) record) << out;
        for (const auto& kv : *record->getAsObject()) {
            EXPECT_NE(props->get(kv.first), nullptr)
                << "a diagnostic writes `" << kv.first.str() << "` but the schema does not document it";
        }
    }
}

// End to end: a lint of a project file reports `at: source` beside the unchanged location.
TEST(DiagnosticLocation, aLintedStoreErrorIsAtSource) {
    std::string bin = (sourceRoot() / "build" / "src" / "cajeta").string();
    if (!fs::exists(bin)) GTEST_SKIP() << "no compiler binary";
    static std::mt19937_64 rng(std::random_device{}());
    auto root = fs::temp_directory_path() / ("cajeta_loc_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "test");
    std::ofstream(root / "src" / "test" / "Keep.cajeta")
        << "package test;\n"
           "public class Cell {\n    public int64 n;\n    public Cell(int64 v) { this.n = v; }\n}\n"
           "public final class Keep {\n    public Cell c;\n    public Keep() { }\n"
           "    public void set(Cell p) { this.c = p; }\n}\n";
    auto out = root / "out.txt";
    std::string cmd = bin + " --lint " + (root / "src").string() + " --emit-xref="
                    + (root / "x.json").string() + " --diag-format=json > " + out.string() + " 2>&1";
    (void) std::system(cmd.c_str());
    std::ifstream f(out);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(has(text, "\"file\":\"test/Keep.cajeta\",\"line\":9,\"column\":31")) << text;
    EXPECT_TRUE(has(text, "\"at\":\"source\"")) << text;
}
