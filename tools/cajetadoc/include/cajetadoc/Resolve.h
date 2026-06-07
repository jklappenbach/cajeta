// cajetadoc — cross-reference resolution (plan §5).
//
// Builds a symbol index from the declaration model and resolves reference
// targets — bracket links (`[String]`, `[String.length]`, `[Hash#identity]`),
// fully-qualified names, and simple names in scope — into stable hyperlinks to
// the generated pages (page + member anchor). Unresolved refs return nullopt so
// the caller can degrade to plain text.
#ifndef CAJETADOC_RESOLVE_H
#define CAJETADOC_RESOLVE_H

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "cajetadoc/Model.h"

namespace cajetadoc {

// A resolved reference: a root-relative page path plus an optional member anchor.
struct ResolvedRef {
    std::string page;   // e.g. "cajeta/hash/Hash.html"
    std::string anchor; // e.g. "identity", or "" for a type-level link
};

// Sanitized in-page anchor for a member name (shared by the index and renderer
// so produced anchors and resolved anchors always agree).
std::string memberAnchor(const std::string& name);

class SymbolIndex {
public:
    void build(const Model& model);

    // Resolve a reference target (e.g. "String", "cajeta.hash.Hash",
    // "Hash.identity(pointer)", "String#length") relative to the package the
    // documented declaration lives in. Returns nullopt when unresolved or
    // ambiguous.
    std::optional<ResolvedRef> resolve(const std::string& target,
                                       const std::string& currentPackage) const;

    // Build an href to `r` from a page nested `currentDepth` directories below
    // the output root (0 = the root overview page).
    std::string href(const ResolvedRef& r, int currentDepth) const;

    bool empty() const { return fqnToPage_.empty(); }

private:
    void addType(const Type& t, const std::string& pkg, const std::string& outerPrefix,
                 const std::string& pkgPath);
    // Resolve just the type part to (fqn, page). Honors FQN, then simple name in
    // scope (preferring currentPackage; ambiguous → nullopt).
    std::optional<std::pair<std::string, std::string>> resolveType(
        const std::string& name, const std::string& currentPackage) const;

    std::unordered_map<std::string, std::string> fqnToPage_;             // FQN -> page
    std::unordered_map<std::string, std::vector<std::string>> simpleToFqn_; // simple -> FQNs
    std::unordered_map<std::string, std::set<std::string>> typeMembers_;   // FQN -> members
};

} // namespace cajetadoc

#endif // CAJETADOC_RESOLVE_H
