// Coverage map parsing, threshold enforcement, and multi-format report emission.
// Map wire format, ASCII, one line per source file: `<relpath> <covered> <total>`;
// blank and `#` lines are ignored, and `# cajeta-coverage-map v1 grain=line` leads.

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

    // Parse a coverage-map file's text into a typed map; errors cite the line number.
    llvm::Expected<CoverageMap> parseCoverageMap(const std::string& text);

    // Keep only the entries matching NO pattern; the input map is unchanged. `*`
    // matches any chars except '/', `**` any chars including '/', and anything else
    // is a literal substring.
    CoverageMap applyExcludes(const CoverageMap& m,
                              const std::vector<std::string>& patterns);

    // Compute the overall coverage percentage across the map.
    double overallPercent(const CoverageMap& m);

    // Sort + return the bottom-N files by percentage (lowest first).
    std::vector<CoverageFile> bottomN(const CoverageMap& m, size_t n);

    // Render reports to disk, `reports` being (format, path) pairs; the formats are
    // "html", "sarif", "lcov" and "console", which writes plain text to its path.
    llvm::Error renderCoverageReports(
        const CoverageMap& m,
        const std::map<std::string, std::string>& reports);

    // The console-summary string, which the test action surfaces on stderr when the
    // format is "console" with no path: `coverage: 86.42% (412/487 over N files, ...)`.
    std::string consoleSummary(const CoverageMap& m);

    // Threshold gate result; when `violated`, `detail` is a bottom-N citation ready
    // to surface as the test action's error message.
    struct ThresholdResult {
        bool violated = false;
        std::string detail;
    };

    // Check thresholds. A negative `minOverall` or `minPerFile` skips that check, so
    // both negative is a no-op returning {violated=false}.
    ThresholdResult checkThresholds(const CoverageMap& m,
                                    double minOverall,
                                    double minPerFile,
                                    size_t bottomNSize = 5);

} // namespace cajeta::buildtool
