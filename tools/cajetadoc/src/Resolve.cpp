#include "cajetadoc/Resolve.h"

#include <algorithm>
#include <cctype>

namespace cajetadoc {

namespace {
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string pkgPathOf(const std::string& pkg) {
    std::string p = pkg;
    std::replace(p.begin(), p.end(), '.', '/');
    return p;
}
} // namespace

std::string memberAnchor(const std::string& name) {
    std::string a = name;
    for (char& c : a)
        if (!std::isalnum((unsigned char)c)) c = '-';
    return a;
}

void SymbolIndex::addType(const Type& t, const std::string& pkg, const std::string& outerPrefix,
                          const std::string& pkgPath) {
    std::string fqn = (pkg.empty() ? "" : pkg + ".") + outerPrefix + t.name;
    std::string pageName = outerPrefix + t.name; // "Hash" or "Outer.Inner"
    std::string page = (pkgPath.empty() ? "" : pkgPath + "/") + pageName + ".html";
    fqnToPage_[fqn] = page;
    simpleToFqn_[t.name].push_back(fqn);
    auto& members = typeMembers_[fqn];
    for (const auto& m : t.members) members.insert(m.name);
    for (const auto& n : t.nested) addType(n, pkg, outerPrefix + t.name + ".", pkgPath);
}

void SymbolIndex::build(const Model& model) {
    for (const auto& p : model.packages) {
        std::string pkgPath = pkgPathOf(p.name);
        for (const auto& t : p.types) addType(t, p.name, "", pkgPath);
    }
}

std::optional<std::pair<std::string, std::string>> SymbolIndex::resolveType(
    const std::string& name, const std::string& currentPackage) const {
    auto fit = fqnToPage_.find(name);
    if (fit != fqnToPage_.end()) return std::make_pair(name, fit->second);

    auto sit = simpleToFqn_.find(name);
    if (sit == simpleToFqn_.end() || sit->second.empty()) return std::nullopt;
    const std::vector<std::string>& cands = sit->second;
    if (cands.size() == 1) {
        return std::make_pair(cands[0], fqnToPage_.at(cands[0]));
    }
    // ambiguous: prefer a candidate in the current package
    std::string match;
    int matches = 0;
    for (const std::string& fqn : cands) {
        // package = fqn with the trailing ".<name>" removed
        std::string pkg;
        size_t dot = fqn.rfind('.');
        if (dot != std::string::npos) pkg = fqn.substr(0, dot);
        if (pkg == currentPackage) { match = fqn; ++matches; }
    }
    if (matches == 1) return std::make_pair(match, fqnToPage_.at(match));
    return std::nullopt; // genuinely ambiguous
}

std::optional<ResolvedRef> SymbolIndex::resolve(const std::string& target,
                                                const std::string& currentPackage) const {
    std::string s = trim(target);
    if (s.empty()) return std::nullopt;

    std::string typePart, memberPart;
    bool hasMember = false;

    size_t hash = s.find('#');
    if (hash != std::string::npos) {
        typePart = trim(s.substr(0, hash));
        memberPart = trim(s.substr(hash + 1));
        hasMember = true;
    } else {
        // whole string as a type first (handles "cajeta.hash.Hash" and "Hash")
        if (auto whole = resolveType(s, currentPackage))
            return ResolvedRef{whole->second, ""};
        size_t dot = s.rfind('.');
        if (dot == std::string::npos) return std::nullopt;
        typePart = trim(s.substr(0, dot));
        memberPart = trim(s.substr(dot + 1));
        hasMember = true;
    }

    auto ty = resolveType(typePart, currentPackage);
    if (!ty) return std::nullopt;
    if (!hasMember || memberPart.empty()) return ResolvedRef{ty->second, ""};

    // strip call args for the anchor: "identity(pointer)" -> "identity"
    std::string mname = memberPart;
    size_t paren = mname.find('(');
    if (paren != std::string::npos) mname = trim(mname.substr(0, paren));

    auto mit = typeMembers_.find(ty->first);
    if (mit != typeMembers_.end() && mit->second.count(mname))
        return ResolvedRef{ty->second, memberAnchor(mname)};
    // member not found — still a useful link to the type page
    return ResolvedRef{ty->second, ""};
}

std::string SymbolIndex::href(const ResolvedRef& r, int currentDepth) const {
    std::string up;
    for (int i = 0; i < currentDepth; ++i) up += "../";
    std::string h = up + r.page;
    if (!r.anchor.empty()) h += "#" + r.anchor;
    return h;
}

} // namespace cajetadoc
