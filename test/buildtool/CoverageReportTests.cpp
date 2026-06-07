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

TEST(CoverageReportTests, parseSimpleMap) {
    auto m = parseCoverageMap(R"(# cajeta-coverage-map v1 grain=line
src/a.cajeta 80 100
src/b.cajeta 50 100
src/c.cajeta 100 100
)");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    EXPECT_EQ(m->grain, "line");
    ASSERT_EQ(m->files.size(), 3u);
    EXPECT_EQ(m->files[0].path, "src/a.cajeta");
    EXPECT_EQ(m->files[0].covered, 80);
    EXPECT_EQ(m->files[0].total, 100);
}

TEST(CoverageReportTests, parseBlankLinesAndCommentsIgnored) {
    auto m = parseCoverageMap(R"(
# top
src/a.cajeta 1 2

# middle
src/b.cajeta 3 4
)");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    ASSERT_EQ(m->files.size(), 2u);
}

TEST(CoverageReportTests, parseMalformedLineErrorsWithCitation) {
    auto m = parseCoverageMap(R"(src/a.cajeta not a number
)");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("line 1"), std::string::npos);
    EXPECT_NE(msg.find("malformed"), std::string::npos);
}

TEST(CoverageReportTests, parseRejectsCoveredGreaterThanTotal) {
    auto m = parseCoverageMap("src/a.cajeta 200 100\n");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("out of range"), std::string::npos);
}

TEST(CoverageReportTests, parseBranchGrainHeader) {
    auto m = parseCoverageMap(
        "# cajeta-coverage-map v1 grain=branch\nsrc/a.cajeta 1 1\n");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    EXPECT_EQ(m->grain, "branch");
}

// ─── exclude globs ──────────────────────────────────────────

TEST(CoverageReportTests, excludeFiltersByDoubleStar) {
    auto m = parseCoverageMap(
        "src/main.cajeta 1 2\n"
        "test/foo.cajeta 1 1\n"
        "test/sub/bar.cajeta 0 1\n");
    ASSERT_TRUE((bool)m);
    auto filtered = applyExcludes(*m, {"test/**"});
    ASSERT_EQ(filtered.files.size(), 1u);
    EXPECT_EQ(filtered.files[0].path, "src/main.cajeta");
}

TEST(CoverageReportTests, excludeNoPatternsReturnsAllFiles) {
    auto m = parseCoverageMap("src/a.cajeta 1 2\n");
    ASSERT_TRUE((bool)m);
    auto out = applyExcludes(*m, {});
    EXPECT_EQ(out.files.size(), 1u);
}

// ─── overall + bottom-N ─────────────────────────────────────

TEST(CoverageReportTests, overallPercentWeightsByTotal) {
    CoverageMap m;
    m.files = {
        {"a", 8,  10},      //  80%, weight 10
        {"b", 92, 100},     //  92%, weight 100
    };
    // (8+92) / (10+100) = 100/110 ≈ 90.909
    EXPECT_NEAR(overallPercent(m), 90.909, 0.01);
}

TEST(CoverageReportTests, bottomNReturnsWorstFirst) {
    CoverageMap m;
    m.files = {
        {"hi.cajeta", 100, 100},   // 100%
        {"low.cajeta",  5, 100},   //   5%
        {"mid.cajeta", 50, 100},   //  50%
    };
    auto bn = bottomN(m, 2);
    ASSERT_EQ(bn.size(), 2u);
    EXPECT_EQ(bn[0].path, "low.cajeta");
    EXPECT_EQ(bn[1].path, "mid.cajeta");
}

// ─── threshold gates ────────────────────────────────────────

TEST(CoverageReportTests, thresholdOk) {
    CoverageMap m;
    m.files = {
        {"a", 90, 100},
        {"b", 95, 100},
    };
    auto r = checkThresholds(m, 80.0, 80.0);
    EXPECT_FALSE(r.violated);
}

TEST(CoverageReportTests, minOverallViolationCitesBottomN) {
    CoverageMap m;
    m.files = {
        {"a.cajeta",  20, 100},
        {"b.cajeta",  60, 100},
        {"c.cajeta", 100, 100},
    };
    // overall = (20 + 60 + 100) / 300 = 60%
    auto r = checkThresholds(m, 80.0, -1.0);
    EXPECT_TRUE(r.violated);
    EXPECT_NE(r.detail.find("overall 60"), std::string::npos);
    EXPECT_NE(r.detail.find("a.cajeta"), std::string::npos)
        << r.detail;
    EXPECT_NE(r.detail.find("bottom"), std::string::npos);
}

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

TEST(CoverageReportTests, htmlEmitterIncludesAllFiles) {
    CoverageMap m;
    m.grain = "line";
    m.files = {
        {"src/<unsafe>.cajeta", 30, 60},
        {"src/safe.cajeta", 60, 60},
    };
    auto d = tempDir("html");
    auto path = (d / "cov.html").string();
    auto err = renderCoverageReports(m, {{"html", path}});
    ASSERT_FALSE((bool)err) << errorText(std::move(err));
    auto contents = readFile(path);
    EXPECT_NE(contents.find("<!doctype html>"), std::string::npos);
    // HTML-escaped path entry.
    EXPECT_NE(contents.find("src/&lt;unsafe&gt;.cajeta"),
              std::string::npos);
    EXPECT_NE(contents.find("src/safe.cajeta"), std::string::npos);
    std::filesystem::remove_all(d);
}

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

TEST(CoverageReportTests, lcovEmitterRecordsCounts) {
    CoverageMap m;
    m.files = {
        {"a.cajeta", 12, 20},
    };
    auto d = tempDir("lcov");
    auto path = (d / "cov.info").string();
    auto err = renderCoverageReports(m, {{"lcov", path}});
    ASSERT_FALSE((bool)err) << errorText(std::move(err));
    auto contents = readFile(path);
    EXPECT_NE(contents.find("SF:a.cajeta"), std::string::npos);
    EXPECT_NE(contents.find("LH:12"), std::string::npos);
    EXPECT_NE(contents.find("LF:20"), std::string::npos);
    EXPECT_NE(contents.find("end_of_record"), std::string::npos);
    std::filesystem::remove_all(d);
}

TEST(CoverageReportTests, consoleSummaryShape) {
    CoverageMap m;
    m.grain = "line";
    m.files = {{"a", 5, 10}};
    auto s = consoleSummary(m);
    EXPECT_NE(s.find("coverage: 50.00%"), std::string::npos);
    EXPECT_NE(s.find("grain=line"), std::string::npos);
}

TEST(CoverageReportTests, unknownFormatErrorsClearly) {
    CoverageMap m;
    auto err = renderCoverageReports(m, {{"junit", "/tmp/x"}});
    ASSERT_TRUE((bool)err);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("unknown report format"), std::string::npos);
    EXPECT_NE(msg.find("junit"), std::string::npos);
}
