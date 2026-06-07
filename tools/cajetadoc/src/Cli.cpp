// cajetadoc — shared CLI implementation (plan §1.2.1 / §14, first pass).
//
// Drives both the standalone `cajetadoc` binary and `cajeta doc`.
//
// Usage:
//   <prog> <source-root> [-o <out-dir>] [options]
//   <prog> <source-root> --emit-model-json     (dump the doc model as JSON)
//   <prog> --render-md                          (render Markdown from stdin)
//
// Options:
//   -o, --output <dir>      output directory (default: build/docs)
//   --emit-model-json       print the declaration model as JSON and exit
//   --include-private       include private declarations
//   --include-internal      include package-private/internal declarations
//   --exclude-dir <name>    skip a directory by name (repeatable)
#include "cajetadoc/Cli.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cajetadoc/Ingest.h"
#include "cajetadoc/Json.h"
#include "cajetadoc/Markdown.h"
#include "cajetadoc/Render.h"

namespace cajetadoc {

namespace {
void usage(const char* prog) {
    std::cerr <<
        "cajetadoc — cajeta documentation generator\n\n"
        "Usage:\n"
        "  " << prog << " <source-root> [-o <out-dir>] [options]\n"
        "  " << prog << " <source-root> --emit-model-json\n\n"
        "Options:\n"
        "  -o, --output <dir>    output directory (default: build/docs)\n"
        "  --emit-model-json     print the declaration model as JSON and exit\n"
        "  --include-private     include private declarations\n"
        "  --include-internal    include package-private/internal declarations\n"
        "  --exclude-dir <name>  skip a directory by name (repeatable)\n"
        "  --project-title <s>   header title (default: Cajeta)\n"
        "  --project-version <s> header version, shown as v<s>\n"
        "  --date-published <s>  header publish date\n"
        "  --project-license <s> header license type\n"
        "  --project-icon <url>  header icon (default: built-in placeholder)\n";
}
} // namespace

int runCli(int argc, const char* const* argv) {
    const char* prog = (argc > 0 && argv[0]) ? argv[0] : "cajetadoc";
    std::string sourceRoot;
    std::string outDir = "build/docs";
    bool emitModel = false;
    IngestOptions opts;
    SiteMeta meta;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(prog); return 0; }
        else if (a == "--render-md") {
            // Debug/golden-bless helper: render Markdown from stdin to HTML.
            std::ostringstream ss;
            ss << std::cin.rdbuf();
            std::cout << renderMarkdown(ss.str());
            return 0;
        }
        else if (a == "-o" || a == "--output") { if (i + 1 < argc) outDir = argv[++i]; }
        else if (a == "--emit-model-json") emitModel = true;
        else if (a == "--include-private") opts.includePrivate = true;
        else if (a == "--include-internal") opts.includeInternal = true;
        else if (a == "--exclude-dir") { if (i + 1 < argc) opts.excludeDirs.push_back(argv[++i]); }
        else if (a == "--project-title") { if (i + 1 < argc) meta.title = argv[++i]; }
        else if (a == "--project-version") { if (i + 1 < argc) meta.version = argv[++i]; }
        else if (a == "--date-published") { if (i + 1 < argc) meta.datePublished = argv[++i]; }
        else if (a == "--project-license") { if (i + 1 < argc) meta.license = argv[++i]; }
        else if (a == "--project-icon") { if (i + 1 < argc) meta.iconHref = argv[++i]; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << prog << ": unknown option " << a << "\n";
            usage(prog);
            return 2;
        } else if (sourceRoot.empty()) {
            sourceRoot = a;
        } else {
            std::cerr << prog << ": unexpected argument " << a << "\n";
            return 2;
        }
    }

    if (sourceRoot.empty()) { usage(prog); return 2; }

    IngestResult res = ingestTree(sourceRoot, opts);
    for (const auto& d : res.diagnostics)
        std::cerr << prog << ": " << d.file << ":" << d.line << ": " << d.message << "\n";

    if (emitModel) {
        std::cout << toJson(res.model);
        return 0;
    }

    std::string error;
    int pages = generateSite(res.model, outDir, error, meta);
    if (!error.empty()) {
        std::cerr << prog << ": " << error << "\n";
        return 1;
    }
    std::cerr << prog << ": parsed " << res.filesParsed << " file(s), "
              << res.model.typeCount() << " type(s); wrote " << pages << " page(s) to "
              << outDir << "\n";
    return 0;
}

} // namespace cajetadoc
