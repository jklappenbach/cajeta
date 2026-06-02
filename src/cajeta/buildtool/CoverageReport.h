// Phase 7 — Coverage map parsing, threshold enforcement, and
// multi-format report emission.
//
// The `test` action invokes the cajeta.coverage plugin when the
// task is configured with a coverage block. The plugin writes a
// coverage map file (simple text format: one line per source file)
// and the test action consumes it here:
//   - parses + validates the map
//   - applies `exclude` glob patterns to drop fixtures / generated
//     sources / opt-out files from the denominator
//   - computes overall + per-file coverage percentage
//   - checks `min` (overall floor) + `min-per-file` (worst-file floor)
//   - on violation, emits a bottom-N citation (worst N files)
//   - writes report files in the requested formats (HTML, console,
//     SARIF, lcov)
//
// Coverage map wire format (one line per source file, ASCII):
//
//     <relpath> <covered-counters> <total-counters>
//
// Example:
//     src/cajeta/buildtool/Resolver.cajeta 412 487
//     src/cajeta/buildtool/Manifest.cajeta 199 199
//
// Blank lines + `#`-prefixed comments are ignored. The map's grain
// (line / branch / region) lives in a single header comment so
// reports can label themselves correctly:
//
//     # cajeta-coverage-map v1 grain=line
//
// Any other shape errors out with the offending line cited.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct CoverageFile {
        std::string path;       // relative to project root
        int64_t covered = 0;
        int64_t total = 0;
        double percent() const {
            return total == 0 ? 100.0
                              : 100.0 * static_cast<double>(covered) /
                                       static_cast<double>(total);
        }
    };

    struct CoverageMap {
        std::string grain;          // "line" | "branch" | "region"
        std::vector<CoverageFile> files;
    };

    // Parse a coverage-map file's text contents into a typed map.
    // Errors cite the line number for diagnosability.
    llvm::Expected<CoverageMap> parseCoverageMap(const std::string& text);

    // Apply glob-style exclude patterns to the map. A file matches
    // when its path matches ANY pattern. The original map is
    // returned unmodified; the result holds only the kept entries.
    // Patterns:
    //   "*"   — any chars except '/'
    //   "**"  — any chars including '/'
    //   exact substrings — literal match
    CoverageMap applyExcludes(const CoverageMap& m,
                              const std::vector<std::string>& patterns);

    // Compute the overall coverage percentage across the map.
    double overallPercent(const CoverageMap& m);

    // Sort + return the bottom-N files by percentage (lowest first).
    std::vector<CoverageFile> bottomN(const CoverageMap& m, size_t n);

    // Render reports to disk. Each entry in `reports` is a
    // (format, path) pair; supported formats: "html", "sarif",
    // "lcov", "console" (writes to path as plain text).
    llvm::Error renderCoverageReports(
        const CoverageMap& m,
        const std::map<std::string, std::string>& reports);

    // Render the console-summary string (used by the test action
    // for stderr surfacing when format == "console" *and* no path
    // is configured). Format:
    //   coverage: 86.42% (412/487 over N files, grain=line)
    std::string consoleSummary(const CoverageMap& m);

    // Threshold gate result. When `violated` is true, `detail`
    // contains a bottom-N citation suitable for surfacing as the
    // test action's error message.
    struct ThresholdResult {
        bool violated = false;
        std::string detail;
    };

    // Check thresholds. `minOverall < 0` skips the overall check;
    // `minPerFile < 0` skips the per-file check. Both negative is
    // a no-op (returns {violated=false}).
    ThresholdResult checkThresholds(const CoverageMap& m,
                                    double minOverall,
                                    double minPerFile,
                                    size_t bottomNSize = 5);

} // namespace cajeta::buildtool
