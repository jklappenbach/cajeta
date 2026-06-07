// cajetadoc — source ingestion (plan §2).
//
// Walk a cajeta source tree, parse each `.cajeta` file with the *real*
// compiler front end (the antlr-generated CajetaLexer/CajetaParser, reused
// verbatim from generated/), and build the declaration Model. Doc comments
// are recovered from the hidden token channel by source position and paired
// with the declaration they immediately precede.
#ifndef CAJETADOC_INGEST_H
#define CAJETADOC_INGEST_H

#include <string>
#include <vector>

#include "cajetadoc/Model.h"

namespace cajetadoc {

struct IngestDiagnostic {
    std::string file;
    int line = 0;
    std::string message;
};

struct IngestResult {
    Model model;
    std::vector<IngestDiagnostic> diagnostics;
    int filesParsed = 0;
};

struct IngestOptions {
    bool includePrivate = false;
    bool includeInternal = false; // package-private / internal
    // Directory names to skip entirely (e.g. the XPU packages owned by a
    // separate session — plan §17).
    std::vector<std::string> excludeDirs;
};

// Ingest a single `.cajeta` file's text into the model.
// `relPath` is the path stored in SourceRef (relative to the source root).
void ingestSource(const std::string& text, const std::string& relPath,
                  const IngestOptions& opts, IngestResult& out);

// Walk a directory tree, ingesting every `.cajeta` file found.
IngestResult ingestTree(const std::string& rootDir, const IngestOptions& opts);

} // namespace cajetadoc

#endif // CAJETADOC_INGEST_H
