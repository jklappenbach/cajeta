#include "cajetadoc/Render.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cajetadoc/DocComment.h"
#include "cajetadoc/Markdown.h"
#include "cajetadoc/Resolve.h"

namespace fs = std::filesystem;

namespace cajetadoc {

namespace {

int packageDepth(const std::string& pkg) {
    if (pkg.empty()) return 0;
    int n = 1;
    for (char c : pkg) if (c == '.') ++n;
    return n;
}

std::string upPath(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i) s += "../";
    return s;
}

std::string relCssFromDepth(int depth) { return upPath(depth) + "cajetadoc.css"; }

std::string pkgPath(const std::string& pkg) {
    std::string p = pkg;
    std::replace(p.begin(), p.end(), '.', '/');
    return p;
}

std::string trimStr(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Markdown options for a page: heading offset + a cross-reference link resolver
// bound to this page's package and depth.
MarkdownOptions pageOpts(const SymbolIndex* index, const std::string& currentPackage) {
    MarkdownOptions opts;
    opts.headingOffset = 2; // class doc owns <h1>; comment ## -> <h4>
    if (index && !index->empty()) {
        int depth = packageDepth(currentPackage);
        const SymbolIndex* idx = index;
        std::string pkg = currentPackage;
        opts.linkResolver = [idx, pkg, depth](const std::string& target) -> std::string {
            auto r = idx->resolve(target, pkg);
            if (!r) return "";
            return idx->href(*r, depth);
        };
    }
    return opts;
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

std::string renderBody(const DocComment* doc, const MarkdownOptions& opts) {
    if (!doc) return "";
    return renderMarkdown(doc->body, opts);
}

std::string renderSummary(const DocComment* doc, const MarkdownOptions& opts) {
    if (!doc || doc->summary.empty()) return "";
    return renderInline(doc->summary, opts);
}

const char* kindLabel(TypeKind k) { return toString(k); }

std::string structuredBadges(const DocComment* doc) {
    if (!doc) return "";
    std::ostringstream os;
    auto add = [&](const char* cls, const std::string& label) {
        os << "<span class=\"badge badge-" << cls << "\">" << htmlEscape(label) << "</span>";
    };
    for (const BlockTag* t : doc->tags("Complexity"))
        add("complexity", t->body.empty() ? std::string("Complexity") : t->body);
    if (!doc->tags("EntryPoint").empty()) add("entry", "entry point");
    if (!doc->tags("FiberSafe").empty()) add("fiber", "fiber-safe");
    if (!doc->tags("FiberUnsafe").empty()) add("fiber-unsafe", "fiber-unsafe");
    if (!doc->tags("Blocks").empty()) add("blocks", "blocks");
    if (!doc->tags("NonBlocking").empty()) add("nonblocking", "non-blocking");
    std::string s = os.str();
    return s.empty() ? s : "<span class=\"badges\">" + s + "</span>";
}

// Render a "See Also" entry: resolve a bare reference to a link, else fall back
// to inline Markdown (so `@See [text](url)` still works).
std::string renderSeeEntry(const std::string& body, const MarkdownOptions& opts) {
    std::string b = trimStr(body);
    if (!b.empty() && b[0] != '[' && b[0] != '<' && b.find(' ') == std::string::npos &&
        opts.linkResolver) {
        std::string href = opts.linkResolver(b);
        if (!href.empty())
            return "<a href=\"" + htmlEscape(href) + "\"><code>" + htmlEscape(b) +
                   "</code></a>";
    }
    return renderInline(body, opts);
}

// Standard JavaDoc block-tag widgets. Works for both type-level and member-level
// docs (a type simply has no @Param/@Return). `params` controls whether the
// parameter/return/throws widgets are emitted (only meaningful for members).
std::string blockTagWidgets(const DocComment* doc, const MarkdownOptions& opts, bool members) {
    if (!doc) return "";
    std::ostringstream os;

    if (members) {
        auto params = doc->tags("Param");
        if (!params.empty()) {
            os << "<div class=\"tag-block\"><h4>Parameters</h4><table class=\"params\">";
            for (const BlockTag* t : params)
                os << "<tr><td class=\"pname\"><code>" << htmlEscape(t->arg) << "</code></td><td>"
                   << renderInline(t->body, opts) << "</td></tr>";
            os << "</table></div>\n";
        }
        auto ret = doc->tags("Return");
        if (ret.empty()) ret = doc->tags("returns"); // tolerate the @returns variant
        if (!ret.empty())
            os << "<div class=\"tag-block\"><h4>Returns</h4><p>"
               << renderInline(ret.front()->body, opts) << "</p></div>\n";
        std::vector<const BlockTag*> throwsT = doc->tags("Throws");
        for (const BlockTag* t : doc->tags("Exception")) throwsT.push_back(t);
        if (!throwsT.empty()) {
            os << "<div class=\"tag-block\"><h4>Throws</h4><table class=\"params\">";
            for (const BlockTag* t : throwsT)
                os << "<tr><td class=\"pname\">" << renderSeeEntry(t->arg, opts) << "</td><td>"
                   << renderInline(t->body, opts) << "</td></tr>";
            os << "</table></div>\n";
        }
    }

    auto see = doc->tags("See");
    if (!see.empty()) {
        os << "<div class=\"tag-block\"><h4>See Also</h4><ul class=\"see\">";
        for (const BlockTag* t : see) os << "<li>" << renderSeeEntry(t->body, opts) << "</li>";
        os << "</ul></div>\n";
    }
    std::ostringstream meta;
    auto since = doc->tags("Since");
    if (!since.empty())
        meta << "<p class=\"since\"><strong>Since:</strong> "
             << renderInline(since.front()->body, opts) << "</p>\n";
    auto author = doc->tags("Author");
    for (const BlockTag* t : author)
        meta << "<p class=\"author\"><strong>Author:</strong> " << renderInline(t->body, opts)
             << "</p>\n";
    auto version = doc->tags("Version");
    if (!version.empty())
        meta << "<p class=\"version\"><strong>Version:</strong> "
             << renderInline(version.front()->body, opts) << "</p>\n";
    os << meta.str();
    return os.str();
}

void renderMemberDetail(std::ostringstream& os, const Member& m, const MarkdownOptions& opts) {
    bool deprecated = m.doc && !m.doc->tags("Deprecated").empty();
    os << "<section class=\"member" << (deprecated ? " deprecated" : "") << "\" id=\""
       << htmlEscape(memberAnchor(m.name)) << "\">\n";
    os << "<h3 class=\"member-sig\"><code>" << htmlEscape(m.signature()) << "</code>"
       << structuredBadges(m.doc.get()) << "</h3>\n";
    if (deprecated)
        os << "<div class=\"deprecation\"><strong>Deprecated.</strong> "
           << renderInline(m.doc->tags("Deprecated").front()->body, opts) << "</div>\n";
    std::string body = renderBody(m.doc.get(), opts);
    if (!body.empty()) os << "<div class=\"member-doc\">" << body << "</div>\n";
    os << blockTagWidgets(m.doc.get(), opts, /*members=*/true);
    os << "</section>\n";
}

// Breadcrumb trail: Overview › package › Type.
std::string breadcrumbs(const std::string& pkg, const std::string& typeName, int depth) {
    std::string up = upPath(depth);
    std::ostringstream os;
    os << "<nav class=\"crumbs\"><a href=\"" << up << "index.html\">Overview</a>";
    if (!pkg.empty())
        os << " <span class=\"sep\">/</span> <a href=\"" << up << pkgPath(pkg)
           << "/index.html\"><code>" << htmlEscape(pkg) << "</code></a>";
    if (!typeName.empty())
        os << " <span class=\"sep\">/</span> <span class=\"crumb-current\">"
           << htmlEscape(typeName) << "</span>";
    os << "</nav>\n";
    return os.str();
}

// Placeholder project icon: a built-in inline SVG (a rounded tile with the
// title's initial). Swapped for a real asset once `meta.iconHref` is supplied.
std::string brandIcon(const SiteMeta& meta) {
    if (!meta.iconHref.empty())
        return "<img class=\"brand-icon\" src=\"" + htmlEscape(meta.iconHref) + "\" alt=\"\">";
    std::string initial(1, meta.title.empty() ? 'C' : meta.title[0]);
    std::ostringstream os;
    os << "<svg class=\"brand-icon\" viewBox=\"0 0 32 32\" aria-hidden=\"true\">"
       << "<rect width=\"32\" height=\"32\" rx=\"7\" fill=\"var(--cajetadoc-accent)\"></rect>"
       << "<text x=\"16\" y=\"22\" text-anchor=\"middle\" font-size=\"18\" font-weight=\"700\" "
          "font-family=\"var(--cajetadoc-mono)\" fill=\"#fff\">"
       << htmlEscape(initial) << "</text></svg>";
    return os.str();
}

// The fixed top header bar present on every page: icon + title left-justified,
// project version/date/license right-justified, with the breadcrumbs on the
// second row. (The prev/next pager lives in the side nav, not here.)
std::string topbar(const SiteMeta& meta, int depth, const std::string& crumbsHtml) {
    std::string up = upPath(depth);
    std::ostringstream os;
    os << "<header class=\"topbar\">\n";
    os << "<div class=\"topbar-brand-row\">\n";
    os << "<a class=\"brand\" href=\"" << up << "index.html\">" << brandIcon(meta)
       << "<span class=\"brand-title\">" << htmlEscape(meta.title) << "</span></a>\n";
    std::ostringstream meta_os;
    bool any = false;
    auto item = [&](const char* cls, const std::string& text) {
        if (text.empty()) return;
        if (any) meta_os << "<span class=\"meta-sep\">·</span>";
        meta_os << "<span class=\"meta-item " << cls << "\">" << htmlEscape(text) << "</span>";
        any = true;
    };
    item("meta-version", meta.version.empty() ? std::string() : "v" + meta.version);
    item("meta-date", meta.datePublished);
    item("meta-license", meta.license);
    os << "<div class=\"project-meta\">" << meta_os.str() << "</div>\n";
    os << "</div>\n"; // .topbar-brand-row
    os << "<div class=\"topbar-nav-row\">\n" << crumbsHtml << "</div>\n";
    os << "</header>\n";
    return os.str();
}

// Sibling-type nav for a type page: every type in the package, current marked.
std::string pkgNav(const Package& pkg, const std::string& currentTypeName) {
    std::vector<const Type*> types;
    for (const auto& t : pkg.types) types.push_back(&t);
    std::sort(types.begin(), types.end(),
              [](const Type* a, const Type* b) { return a->name < b->name; });
    std::ostringstream os;
    os << "<nav class=\"pkg-nav\"><div class=\"pkg-nav-title\"><code>"
       << htmlEscape(pkg.name) << "</code></div>\n<ul>\n";
    for (const Type* t : types) {
        bool cur = t->name == currentTypeName;
        os << "<li" << (cur ? " class=\"active\"" : "") << "><a href=\"" << htmlEscape(t->name)
           << ".html\"><span class=\"kind\">" << kindLabel(t->kind) << "</span> "
           << htmlEscape(t->name) << "</a></li>\n";
    }
    os << "</ul>\n</nav>\n";
    return os.str();
}

// A chevron arrow icon (left or right) for the pager buttons.
std::string pagerIcon(bool left) {
    const char* d = left ? "M10.5 3.5 L5.5 8 L10.5 12.5" : "M5.5 3.5 L10.5 8 L5.5 12.5";
    std::ostringstream os;
    os << "<svg class=\"pager-ico\" viewBox=\"0 0 16 16\" aria-hidden=\"true\"><path d=\"" << d
       << "\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" "
          "stroke-linejoin=\"round\"></path></svg>";
    return os.str();
}

// prev/next pager among the package's (sorted) top-level types. Rendered at the
// top of the left side nav as two icon-only buttons (arrow only, no label) so
// readers can thumb through pages; the active item in the list below shows where
// they are. A direction with no target still renders its button, disabled.
std::string prevNext(const Package& pkg, const std::string& currentTypeName) {
    std::vector<std::string> names;
    for (const auto& t : pkg.types) names.push_back(t.name);
    std::sort(names.begin(), names.end());
    auto it = std::find(names.begin(), names.end(), currentTypeName);
    if (it == names.end()) return "";
    std::ostringstream os;
    os << "<nav class=\"pager\" aria-label=\"type navigation\">";
    if (it != names.begin())
        os << "<a class=\"pager-btn prev\" href=\"" << htmlEscape(*(it - 1))
           << ".html\" title=\"Previous: " << htmlEscape(*(it - 1)) << "\" aria-label=\"Previous: "
           << htmlEscape(*(it - 1)) << "\">" << pagerIcon(true) << "</a>";
    else
        os << "<span class=\"pager-btn prev disabled\" aria-disabled=\"true\" "
              "aria-label=\"No previous type\">" << pagerIcon(true) << "</span>";
    if (it + 1 != names.end())
        os << "<a class=\"pager-btn next\" href=\"" << htmlEscape(*(it + 1))
           << ".html\" title=\"Next: " << htmlEscape(*(it + 1)) << "\" aria-label=\"Next: "
           << htmlEscape(*(it + 1)) << "\">" << pagerIcon(false) << "</a>";
    else
        os << "<span class=\"pager-btn next disabled\" aria-disabled=\"true\" "
              "aria-label=\"No next type\">" << pagerIcon(false) << "</span>";
    os << "</nav>\n";
    return os.str();
}

} // namespace

std::string renderTypePage(const Type& type, const std::string& cssHref, const SymbolIndex* index,
                           const Package* pkg, const SiteMeta& meta) {
    MarkdownOptions opts = pageOpts(index, type.packageName);
    int depth = packageDepth(type.packageName);
    bool deprecated = type.doc && !type.doc->tags("Deprecated").empty();

    std::ostringstream os;
    os << htmlHead(type.qualifiedName(), cssHref);
    os << topbar(meta, depth, breadcrumbs(type.packageName, type.name, depth));
    os << "<div class=\"doc-body\">\n";
    os << "<div class=\"page\">\n";
    if (pkg)
        os << "<aside class=\"sidebar\">\n"
           << prevNext(*pkg, type.name) << pkgNav(*pkg, type.name) << "</aside>\n";
    os << "<main class=\"content\">\n";

    os << "<header class=\"type-header" << (deprecated ? " deprecated" : "") << "\">\n";
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
    auto relList = [&](const char* label, const std::vector<std::string>& xs) {
        if (xs.empty()) return;
        os << "<div class=\"rel\">" << label << " ";
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) os << ", ";
            std::string href;
            if (opts.linkResolver) href = opts.linkResolver(xs[i]);
            if (!href.empty())
                os << "<a href=\"" << htmlEscape(href) << "\"><code>" << htmlEscape(xs[i])
                   << "</code></a>";
            else
                os << "<code>" << htmlEscape(xs[i]) << "</code>";
        }
        os << "</div>\n";
    };
    relList("extends", type.extends);
    relList("implements", type.implements);
    os << "</header>\n";

    if (deprecated)
        os << "<div class=\"deprecation\"><strong>Deprecated.</strong> "
           << renderInline(type.doc->tags("Deprecated").front()->body, opts) << "</div>\n";

    std::string typeBody = renderBody(type.doc.get(), opts);
    if (!typeBody.empty()) os << "<div class=\"type-doc\">" << typeBody << "</div>\n";
    // type-level block tags (@See / @Since / @Author / @Version)
    os << blockTagWidgets(type.doc.get(), opts, /*members=*/false);

    if (!type.members.empty()) {
        os << "<h2>Member Summary</h2>\n<table class=\"summary\">\n";
        for (const auto& m : type.members)
            os << "<tr><td class=\"msig\"><a href=\"#" << htmlEscape(memberAnchor(m.name))
               << "\"><code>" << htmlEscape(m.signature()) << "</code></a></td><td>"
               << renderSummary(m.doc.get(), opts) << "</td></tr>\n";
        os << "</table>\n";
        os << "<h2>Member Detail</h2>\n";
        for (const auto& m : type.members) renderMemberDetail(os, m, opts);
    }

    os << "<footer class=\"cajetadoc-footer\">Generated by cajetadoc</footer>\n";
    os << "</main>\n</div>\n"; // .page
    os << "</div>\n"; // .doc-body
    os << htmlFoot();
    return os.str();
}

std::string renderPackageIndex(const Package& pkg, const std::string& cssHref,
                               const SymbolIndex* index, const SiteMeta& meta) {
    MarkdownOptions opts = pageOpts(index, pkg.name);
    int depth = packageDepth(pkg.name);
    std::string title = pkg.name.empty() ? "(default package)" : pkg.name;
    std::ostringstream os;
    os << htmlHead(title, cssHref);
    os << topbar(meta, depth, breadcrumbs(pkg.name, "", depth));
    os << "<div class=\"doc-body\">\n";
    os << "<main class=\"content\">\n";
    os << "<header class=\"type-header\"><h1><span class=\"kind\">package</span> "
       << htmlEscape(title) << "</h1></header>\n";
    std::string body = renderBody(pkg.doc.get(), opts);
    if (!body.empty()) os << "<div class=\"type-doc\">" << body << "</div>\n";
    os << "<h2>Types</h2>\n<table class=\"summary\">\n";
    std::vector<const Type*> types;
    for (const auto& t : pkg.types) types.push_back(&t);
    std::sort(types.begin(), types.end(),
              [](const Type* a, const Type* b) { return a->name < b->name; });
    for (const Type* t : types)
        os << "<tr><td class=\"msig\"><a href=\"" << htmlEscape(t->name) << ".html\">"
           << "<span class=\"kind\">" << kindLabel(t->kind) << "</span> <code>"
           << htmlEscape(t->name) << "</code></a></td><td>" << renderSummary(t->doc.get(), opts)
           << "</td></tr>\n";
    os << "</table>\n</main>\n</div>\n" << htmlFoot();
    return os.str();
}

std::string renderOverview(const Model& model, const std::string& cssHref,
                           const SymbolIndex* index, const SiteMeta& meta) {
    std::ostringstream os;
    os << htmlHead("API Reference", cssHref);
    os << topbar(meta, /*depth=*/0, breadcrumbs("", "", 0));
    os << "<div class=\"doc-body\">\n";
    os << "<main class=\"content\">\n";
    os << "<header class=\"type-header\"><h1>API Reference</h1></header>\n";
    os << "<h2>Packages</h2>\n<table class=\"summary\">\n";
    std::vector<const Package*> pkgs;
    for (const auto& p : model.packages) if (!p.types.empty()) pkgs.push_back(&p);
    std::sort(pkgs.begin(), pkgs.end(),
              [](const Package* a, const Package* b) { return a->name < b->name; });
    for (const Package* p : pkgs) {
        MarkdownOptions opts = pageOpts(index, p->name);
        std::string path = pkgPath(p->name);
        std::string summary = renderSummary(p->doc.get(), opts);
        if (summary.empty())
            summary = "<span class=\"muted\">" + std::to_string(p->types.size()) + " type" +
                      (p->types.size() == 1 ? "" : "s") + "</span>";
        os << "<tr><td><a href=\"" << htmlEscape(path) << "/index.html\"><code>"
           << htmlEscape(p->name.empty() ? "(default)" : p->name) << "</code></a></td><td>"
           << summary << "</td></tr>\n";
    }
    os << "</table>\n</main>\n</div>\n" << htmlFoot();
    return os.str();
}

std::string renderEntryPointsIndex(const Model& model, const std::string& cssHref,
                                   const SymbolIndex* index, const SiteMeta& meta) {
    std::ostringstream os;
    os << htmlHead("Entry Points", cssHref);
    os << topbar(meta, /*depth=*/0, breadcrumbs("", "", 0));
    os << "<div class=\"doc-body\">\n";
    os << "<main class=\"content\">\n";
    os << "<header class=\"type-header\"><h1>Entry Points</h1></header>\n";
    os << "<p>Methods tagged <code>@EntryPoint</code> — each package's front door.</p>\n";

    std::vector<const Package*> pkgs;
    for (const auto& p : model.packages) if (!p.types.empty()) pkgs.push_back(&p);
    std::sort(pkgs.begin(), pkgs.end(),
              [](const Package* a, const Package* b) { return a->name < b->name; });

    for (const Package* p : pkgs) {
        MarkdownOptions opts = pageOpts(index, p->name);
        std::string path = pkgPath(p->name);
        std::ostringstream body;
        auto emitMember = [&](const Type& t, const Member& m) {
            if (!m.doc || m.doc->tags("EntryPoint").empty()) return;
            body << "<tr><td class=\"msig\"><a href=\"" << htmlEscape(path) << "/"
                 << htmlEscape(t.name) << ".html#" << htmlEscape(memberAnchor(m.name))
                 << "\"><code>" << htmlEscape(t.name) << "." << htmlEscape(m.name)
                 << "</code></a></td><td>" << renderSummary(m.doc.get(), opts)
                 << "</td></tr>\n";
        };
        for (const auto& t : p->types) {
            for (const auto& m : t.members) emitMember(t, m);
            for (const auto& n : t.nested)
                for (const auto& m : n.members) emitMember(n, m);
        }
        std::string rows = body.str();
        if (rows.empty()) continue;
        os << "<h2><code>" << htmlEscape(p->name.empty() ? "(default)" : p->name)
           << "</code></h2>\n<table class=\"summary\">\n" << rows << "</table>\n";
    }

    os << "</main>\n</div>\n" << htmlFoot();
    return os.str();
}

int generateSite(const Model& model, const std::string& outDir, std::string& error,
                 const SiteMeta& meta) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) { error = "cannot create output dir: " + ec.message(); return 0; }

    SymbolIndex index;
    index.build(model);

    {
        std::ofstream css(fs::path(outDir) / "cajetadoc.css");
        if (!css) { error = "cannot write stylesheet"; return 0; }
        css << defaultStylesheet();
    }

    int pages = 0;
    {
        std::ofstream f(fs::path(outDir) / "index.html");
        f << renderOverview(model, "cajetadoc.css", &index, meta);
        ++pages;
    }
    {
        std::ofstream f(fs::path(outDir) / "entry-points.html");
        f << renderEntryPointsIndex(model, "cajetadoc.css", &index, meta);
        ++pages;
    }

    for (const auto& pkg : model.packages) {
        if (pkg.types.empty()) continue;
        std::string rel = pkgPath(pkg.name);
        fs::path dir = fs::path(outDir) / rel;
        fs::create_directories(dir, ec);
        int depth = packageDepth(pkg.name);
        std::string css = relCssFromDepth(depth);

        {
            std::ofstream f(dir / "index.html");
            f << renderPackageIndex(pkg, css, &index, meta);
            ++pages;
        }
        for (const auto& t : pkg.types) {
            std::ofstream f(dir / (t.name + ".html"));
            f << renderTypePage(t, css, &index, &pkg, meta);
            ++pages;
            for (const auto& n : t.nested) {
                std::ofstream nf(dir / (t.name + "." + n.name + ".html"));
                nf << renderTypePage(n, css, &index, &pkg, meta);
                ++pages;
            }
        }
    }
    return pages;
}

} // namespace cajetadoc
