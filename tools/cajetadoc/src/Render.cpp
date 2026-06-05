#include "cajetadoc/Render.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cajetadoc/DocComment.h"
#include "cajetadoc/Markdown.h"

namespace fs = std::filesystem;

namespace cajetadoc {

namespace {

std::string relCssFromDepth(int depth) {
    if (depth <= 0) return "cajetadoc.css";
    std::string s;
    for (int i = 0; i < depth; ++i) s += "../";
    return s + "cajetadoc.css";
}

int packageDepth(const std::string& pkg) {
    if (pkg.empty()) return 0;
    int n = 1;
    for (char c : pkg) if (c == '.') ++n;
    return n;
}

std::string htmlHead(const std::string& title, const std::string& cssHref) {
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
       << "<meta charset=\"utf-8\">\n"
       << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
       << "<title>" << htmlEscape(title) << "</title>\n"
       << "<link rel=\"stylesheet\" href=\"" << htmlEscape(cssHref) << "\">\n"
       << "</head>\n<body>\n<div class=\"cajetadoc\">\n";
    return os.str();
}

std::string htmlFoot() { return "</div>\n</body>\n</html>\n"; }

// Render a doc comment body (Markdown) for a member/type detail block.
std::string renderBody(const DocComment* doc) {
    if (!doc) return "";
    MarkdownOptions opts;
    opts.headingOffset = 3;
    return renderMarkdown(doc->body, opts);
}

std::string renderSummary(const DocComment* doc) {
    if (!doc || doc->summary.empty()) return "";
    return renderInline(doc->summary);
}

const char* kindLabel(TypeKind k) { return toString(k); }

// Render the cajeta-specific structured-tag badges for a member (§8, partial).
std::string structuredBadges(const DocComment* doc) {
    if (!doc) return "";
    std::ostringstream os;
    auto add = [&](const char* cls, const std::string& label) {
        os << "<span class=\"badge badge-" << cls << "\">" << htmlEscape(label) << "</span>";
    };
    for (const BlockTag* t : doc->tags("Complexity"))
        add("complexity", t->body.empty() ? std::string("Complexity") : t->body);
    if (!doc->tags("FiberSafe").empty()) add("fiber", "fiber-safe");
    if (!doc->tags("FiberUnsafe").empty()) add("fiber-unsafe", "fiber-unsafe");
    if (!doc->tags("Blocks").empty()) add("blocks", "blocks");
    if (!doc->tags("NonBlocking").empty()) add("nonblocking", "non-blocking");
    std::string s = os.str();
    return s.empty() ? s : "<span class=\"badges\">" + s + "</span>";
}

// Render standard JavaDoc block tags (params/returns/throws/see/since/deprecated)
// as styled widgets (§6, partial).
std::string blockTagWidgets(const Member& m) {
    const DocComment* doc = m.doc.get();
    if (!doc) return "";
    std::ostringstream os;
    MarkdownOptions inlineOpts;

    auto params = doc->tags("Param");
    if (!params.empty()) {
        os << "<div class=\"tag-block\"><h4>Parameters</h4><table class=\"params\">";
        for (const BlockTag* t : params) {
            os << "<tr><td class=\"pname\"><code>" << htmlEscape(t->arg)
               << "</code></td><td>" << renderInline(t->body, inlineOpts) << "</td></tr>";
        }
        os << "</table></div>\n";
    }
    auto ret = doc->tags("Return");
    if (ret.empty()) ret = doc->tags("returns"); // tolerate the @returns variant
    if (!ret.empty()) {
        os << "<div class=\"tag-block\"><h4>Returns</h4><p>"
           << renderInline(ret.front()->body, inlineOpts) << "</p></div>\n";
    }
    std::vector<const BlockTag*> throwsT = doc->tags("Throws");
    for (const BlockTag* t : doc->tags("Exception")) throwsT.push_back(t);
    if (!throwsT.empty()) {
        os << "<div class=\"tag-block\"><h4>Throws</h4><table class=\"params\">";
        for (const BlockTag* t : throwsT) {
            os << "<tr><td class=\"pname\"><code>" << htmlEscape(t->arg)
               << "</code></td><td>" << renderInline(t->body, inlineOpts) << "</td></tr>";
        }
        os << "</table></div>\n";
    }
    auto see = doc->tags("See");
    if (!see.empty()) {
        os << "<div class=\"tag-block\"><h4>See Also</h4><ul class=\"see\">";
        for (const BlockTag* t : see)
            os << "<li>" << renderInline(t->body, inlineOpts) << "</li>";
        os << "</ul></div>\n";
    }
    auto since = doc->tags("Since");
    if (!since.empty())
        os << "<p class=\"since\"><strong>Since:</strong> "
           << renderInline(since.front()->body, inlineOpts) << "</p>\n";
    return os.str();
}

std::string memberAnchor(const Member& m) {
    std::string a = m.name;
    for (char& c : a) if (!std::isalnum((unsigned char)c)) c = '-';
    return a;
}

void renderMemberDetail(std::ostringstream& os, const Member& m) {
    bool deprecated = m.doc && !m.doc->tags("Deprecated").empty();
    os << "<section class=\"member" << (deprecated ? " deprecated" : "")
       << "\" id=\"" << htmlEscape(memberAnchor(m)) << "\">\n";
    os << "<h3 class=\"member-sig\"><code>" << htmlEscape(m.signature()) << "</code>"
       << structuredBadges(m.doc.get()) << "</h3>\n";
    if (deprecated) {
        os << "<div class=\"deprecation\"><strong>Deprecated.</strong> "
           << renderInline(m.doc->tags("Deprecated").front()->body) << "</div>\n";
    }
    std::string body = renderBody(m.doc.get());
    if (!body.empty()) os << "<div class=\"member-doc\">" << body << "</div>\n";
    os << blockTagWidgets(m);
    os << "</section>\n";
}

} // namespace

std::string renderTypePage(const Type& type, const std::string& cssHref) {
    std::ostringstream os;
    os << htmlHead(type.qualifiedName(), cssHref);

    // header
    os << "<header class=\"type-header\">\n";
    if (!type.packageName.empty())
        os << "<div class=\"pkg-label\">" << htmlEscape(type.packageName) << "</div>\n";
    os << "<h1><span class=\"kind\">" << kindLabel(type.kind) << "</span> "
       << htmlEscape(type.name);
    if (!type.typeParams.empty()) {
        os << "&lt;";
        for (size_t i = 0; i < type.typeParams.size(); ++i) {
            if (i) os << ", ";
            os << htmlEscape(type.typeParams[i].name);
        }
        os << "&gt;";
    }
    os << "</h1>\n";
    if (!type.extends.empty()) {
        os << "<div class=\"rel\">extends ";
        for (size_t i = 0; i < type.extends.size(); ++i) {
            if (i) os << ", ";
            os << "<code>" << htmlEscape(type.extends[i]) << "</code>";
        }
        os << "</div>\n";
    }
    if (!type.implements.empty()) {
        os << "<div class=\"rel\">implements ";
        for (size_t i = 0; i < type.implements.size(); ++i) {
            if (i) os << ", ";
            os << "<code>" << htmlEscape(type.implements[i]) << "</code>";
        }
        os << "</div>\n";
    }
    os << "</header>\n";

    // type description
    std::string typeBody = renderBody(type.doc.get());
    if (!typeBody.empty()) os << "<div class=\"type-doc\">" << typeBody << "</div>\n";

    // member summary
    if (!type.members.empty()) {
        os << "<h2>Member Summary</h2>\n<table class=\"summary\">\n";
        for (const auto& m : type.members) {
            os << "<tr><td class=\"msig\"><a href=\"#" << htmlEscape(memberAnchor(m))
               << "\"><code>" << htmlEscape(m.signature()) << "</code></a></td><td>"
               << renderSummary(m.doc.get()) << "</td></tr>\n";
        }
        os << "</table>\n";

        os << "<h2>Member Detail</h2>\n";
        for (const auto& m : type.members) renderMemberDetail(os, m);
    }

    os << "<footer class=\"cajetadoc-footer\">Generated by cajetadoc</footer>\n";
    os << htmlFoot();
    return os.str();
}

std::string renderPackageIndex(const Package& pkg, const std::string& cssHref) {
    std::ostringstream os;
    std::string title = pkg.name.empty() ? "(default package)" : pkg.name;
    os << htmlHead(title, cssHref);
    os << "<header class=\"type-header\"><h1><span class=\"kind\">package</span> "
       << htmlEscape(title) << "</h1></header>\n";
    std::string body = renderBody(pkg.doc.get());
    if (!body.empty()) os << "<div class=\"type-doc\">" << body << "</div>\n";
    os << "<h2>Types</h2>\n<table class=\"summary\">\n";
    std::vector<const Type*> types;
    for (const auto& t : pkg.types) types.push_back(&t);
    std::sort(types.begin(), types.end(),
              [](const Type* a, const Type* b) { return a->name < b->name; });
    for (const Type* t : types) {
        os << "<tr><td class=\"msig\"><a href=\"" << htmlEscape(t->name) << ".html\">"
           << "<span class=\"kind\">" << kindLabel(t->kind) << "</span> <code>"
           << htmlEscape(t->name) << "</code></a></td><td>" << renderSummary(t->doc.get())
           << "</td></tr>\n";
    }
    os << "</table>\n" << htmlFoot();
    return os.str();
}

std::string renderOverview(const Model& model, const std::string& cssHref) {
    std::ostringstream os;
    os << htmlHead("API Reference", cssHref);
    os << "<header class=\"type-header\"><h1>API Reference</h1></header>\n";
    os << "<h2>Packages</h2>\n<table class=\"summary\">\n";
    std::vector<const Package*> pkgs;
    for (const auto& p : model.packages) if (!p.types.empty()) pkgs.push_back(&p);
    std::sort(pkgs.begin(), pkgs.end(),
              [](const Package* a, const Package* b) { return a->name < b->name; });
    for (const Package* p : pkgs) {
        std::string path = p->name;
        std::replace(path.begin(), path.end(), '.', '/');
        os << "<tr><td><a href=\"" << htmlEscape(path) << "/index.html\"><code>"
           << htmlEscape(p->name.empty() ? "(default)" : p->name) << "</code></a></td><td>"
           << renderSummary(p->doc.get()) << "</td></tr>\n";
    }
    os << "</table>\n" << htmlFoot();
    return os.str();
}

int generateSite(const Model& model, const std::string& outDir, std::string& error) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) { error = "cannot create output dir: " + ec.message(); return 0; }

    // stylesheet at root
    {
        std::ofstream css(fs::path(outDir) / "cajetadoc.css");
        if (!css) { error = "cannot write stylesheet"; return 0; }
        css << defaultStylesheet();
    }

    int pages = 0;
    // overview
    {
        std::ofstream f(fs::path(outDir) / "index.html");
        f << renderOverview(model, "cajetadoc.css");
        ++pages;
    }

    for (const auto& pkg : model.packages) {
        if (pkg.types.empty()) continue;
        std::string rel = pkg.name;
        std::replace(rel.begin(), rel.end(), '.', '/');
        fs::path dir = fs::path(outDir) / rel;
        fs::create_directories(dir, ec);
        int depth = packageDepth(pkg.name);
        std::string css = relCssFromDepth(depth);

        {
            std::ofstream f(dir / "index.html");
            f << renderPackageIndex(pkg, css);
            ++pages;
        }
        for (const auto& t : pkg.types) {
            std::ofstream f(dir / (t.name + ".html"));
            f << renderTypePage(t, css);
            ++pages;
            // nested types get a sibling page
            for (const auto& n : t.nested) {
                std::ofstream nf(dir / (t.name + "." + n.name + ".html"));
                nf << renderTypePage(n, css);
                ++pages;
            }
        }
    }
    return pages;
}

} // namespace cajetadoc
