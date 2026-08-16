// Phase 7 — coverage map parsing, threshold gating, and
// multi-format report emission.
//
// The cajeta.coverage plugin writes a text map (one line per
// source file) which the test action consumes here; this file
// pins:
//
//   - map parser (header grain, comments, ranges, malformed
//     lines)
//   - exclude glob matching
//   - overall percent + bottom-N citation
//   - threshold gates (min overall + min-per-file)
//   - HTML / SARIF / lcov / console emitter shape (we don't
//     re-validate the full SARIF schema — we check the load-
//     bearing fields the BuildTool.md spec lists)

#include "cajeta/buildtool/CoverageReport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::applyExcludes;
using cajeta::buildtool::bottomN;
using cajeta::buildtool::checkThresholds;
using cajeta::buildtool::consoleSummary;
using cajeta::buildtool::CoverageFile;
using cajeta::buildtool::CoverageMap;
using cajeta::buildtool::overallPercent;
using cajeta::buildtool::parseCoverageMap;
using cajeta::buildtool::renderCoverageReports;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-cov-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    std::string readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

} // namespace

// ─── parser ──────────────────────────────────────────────────




TEST(CoverageReportTests, parseRejectsCoveredGreaterThanTotal) {
    auto m = parseCoverageMap("src/a.cajeta 200 100\n");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("out of range"), std::string::npos);
}


// ─── exclude globs ──────────────────────────────────────────



// ─── overall + bottom-N ─────────────────────────────────────



// ─── threshold gates ────────────────────────────────────────



TEST(CoverageReportTests, minPerFileViolationCitesOffenders) {
    CoverageMap m;
    m.files = {
        {"a.cajeta",  20, 100},    //  20% — below floor
        {"b.cajeta",  90, 100},    //  90% — ok
        {"c.cajeta",  40, 100},    //  40% — below floor
    };
    auto r = checkThresholds(m, -1.0, 50.0);
    EXPECT_TRUE(r.violated);
    EXPECT_NE(r.detail.find("per-file floor"), std::string::npos);
    EXPECT_NE(r.detail.find("a.cajeta"), std::string::npos);
    EXPECT_NE(r.detail.find("c.cajeta"), std::string::npos);
    EXPECT_EQ(r.detail.find("b.cajeta"), std::string::npos)
        << r.detail;
}

// ─── report emitters ────────────────────────────────────────


TEST(CoverageReportTests, sarifEmitterCarriesToolMetadataAndResults) {
    CoverageMap m;
    m.grain = "line";
    m.files = {
        {"src/low.cajeta",  10, 100},   // included as "warning"
        {"src/mid.cajeta",  60, 100},   // included as "note"
        {"src/high.cajeta", 95, 100},   // excluded (>= 80%)
    };
    auto d = tempDir("sarif");
    auto path = (d / "cov.sarif").string();
    auto err = renderCoverageReports(m, {{"sarif", path}});
    ASSERT_FALSE((bool)err) << errorText(std::move(err));
    auto contents = readFile(path);
    EXPECT_NE(contents.find("\"version\": \"2.1.0\""),
              std::string::npos);
    EXPECT_NE(contents.find("\"cajeta.coverage\""), std::string::npos);
    EXPECT_NE(contents.find("src/low.cajeta"), std::string::npos);
    EXPECT_NE(contents.find("src/mid.cajeta"), std::string::npos);
    EXPECT_EQ(contents.find("src/high.cajeta"), std::string::npos)
        << "files >= 80% should not appear as SARIF results";
    std::filesystem::remove_all(d);
}



TEST(CoverageReportTests, unknownFormatErrorsClearly) {
    CoverageMap m;
    auto err = renderCoverageReports(m, {{"junit", "/tmp/x"}});
    ASSERT_TRUE((bool)err);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("unknown report format"), std::string::npos);
    EXPECT_NE(msg.find("junit"), std::string::npos);
}
