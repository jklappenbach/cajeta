// cajetadoc — deterministic JSON serialization of the declaration model.
//
// Used by the model-snapshot test harness (plan §1.1.3 / §2.2.1): a stable,
// pretty-printed JSON dump of the doc model, so semantics can be asserted
// without HTML. Deterministic ordering (source order within a type, package
// order as ingested then sorted by name) keeps snapshots stable across runs.
#ifndef CAJETADOC_JSON_H
#define CAJETADOC_JSON_H

#include <string>

namespace cajetadoc {

struct Model;
struct DocComment;

// Pretty-printed (2-space) JSON dump of the whole model.
std::string toJson(const Model& model);

// JSON dump of a single parsed doc comment (for §3 snapshot tests).
std::string toJson(const DocComment& doc);

// Escape a string as a JSON string literal contents (no surrounding quotes).
std::string jsonEscape(const std::string& s);

} // namespace cajetadoc

#endif // CAJETADOC_JSON_H
