// cajetadoc test suite (plan §1 harness + §2/§3/§4/§10 unit & golden tests).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cajetadoc/DocComment.h"
#include "cajetadoc/Ingest.h"
#include "cajetadoc/Json.h"
#include "cajetadoc/Markdown.h"
#include "cajetadoc/Model.h"
#include "cajetadoc/Render.h"
#include "cajetadoc/Resolve.h"

namespace fs = std::filesystem;
using namespace cajetadoc;

#ifndef CAJETADOC_FIXTURES_DIR
#define CAJETADOC_FIXTURES_DIR "."
#endif

namespace {

const Type* findType(const Model& m, const std::string& fqn) {
    for (const auto& p : m.packages)
        for (const auto& t : p.types)
            if (t.qualifiedName() == fqn) return &t;
    return nullptr;
}

const Member* findMember(const Type& t, const std::string& name) {
    for (const auto& mem : t.members)
        if (mem.name == name) return &mem;
    return nullptr;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Golden-file helper (plan §1.1.1): assert produced == expected file contents,
// printing a small diff context on mismatch.
void expectGolden(const std::string& produced, const fs::path& goldenPath) {
    ASSERT_TRUE(fs::exists(goldenPath)) << "missing golden: " << goldenPath;
    std::string expected = slurp(goldenPath);
    if (produced != expected) {
        ADD_FAILURE() << "golden mismatch for " << goldenPath << "\n--- expected ---\n"
                      << expected << "\n--- produced ---\n" << produced;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// §3 — doc-comment parsing
// ---------------------------------------------------------------------------
TEST(DocComment, IsDocComment) {
    EXPECT_TRUE(isDocComment("/** hi */"));
    EXPECT_FALSE(isDocComment("/* hi */"));
    EXPECT_FALSE(isDocComment("// hi"));
    EXPECT_FALSE(isDocComment("/**/"));
}

TEST(DocComment, SummaryAndBodySplit) {
    DocComment d = parseDocComment(
        "/**\n * First sentence here. More body text.\n *\n * @Param x the x\n */");
    EXPECT_EQ(d.summary, "First sentence here.");
    EXPECT_NE(d.body.find("More body text."), std::string::npos);
    ASSERT_EQ(d.blockTags.size(), 1u);
    EXPECT_EQ(d.blockTags[0].name, "Param");
    EXPECT_EQ(d.blockTags[0].arg, "x");
    EXPECT_EQ(d.blockTags[0].body, "the x");
}

TEST(DocComment, GutterStripPreservesFenceIndent) {
    DocComment d = parseDocComment(
        "/**\n * Body.\n *\n * ```\n *     indented\n * ```\n */");
    EXPECT_NE(d.body.find("    indented"), std::string::npos);
}

TEST(DocComment, ReturnTagNoArg) {
    DocComment d = parseDocComment("/**\n * Does a thing.\n * @Return the result value.\n */");
    auto ret = d.tags("Return");
    ASSERT_EQ(ret.size(), 1u);
    EXPECT_EQ(ret[0]->arg, "");
    EXPECT_EQ(ret[0]->body, "the result value.");
}

TEST(DocComment, MultiLineTagBody) {
    DocComment d = parseDocComment(
        "/**\n * S.\n * @Param x first line\n *   second line\n * @Return r\n */");
    auto p = d.tags("Param");
    ASSERT_EQ(p.size(), 1u);
    EXPECT_NE(p[0]->body.find("first line"), std::string::npos);
    EXPECT_NE(p[0]->body.find("second line"), std::string::npos);
    EXPECT_EQ(d.tags("Return").size(), 1u);
}

TEST(DocComment, MalformedInlineTagDiagnostic) {
    DocComment d = parseDocComment("/**\n * See {@Link Unterminated\n */");
    bool found = false;
    for (auto& diag : d.diagnostics)
        if (diag.code == "unterminated-inline-tag") found = true;
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// §2 — ingestion & declaration model
// ---------------------------------------------------------------------------
TEST(Ingest, PackageAndType) {
    IngestResult r;
    ingestSource("package cajeta.demo;\n/** A demo. */\npublic class Demo {}\n",
                 "Demo.cajeta", IngestOptions{}, r);
    Package* p = r.model.findPackage("cajeta.demo");
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->types.size(), 1u);
    EXPECT_EQ(p->types[0].name, "Demo");
    EXPECT_EQ(p->types[0].kind, TypeKind::Class);
    EXPECT_EQ(p->types[0].visibility, Visibility::Public);
    ASSERT_TRUE(p->types[0].doc != nullptr);
    EXPECT_EQ(p->types[0].doc->summary, "A demo.");
}

TEST(Ingest, MemberEnumerationAndDocPairing) {
    IngestResult r;
    ingestTree(std::string(CAJETADOC_FIXTURES_DIR), IngestOptions{}); // smoke
    ingestSource(slurp(fs::path(CAJETADOC_FIXTURES_DIR) / "lang" / "Greeter.cajeta"),
                 "lang/Greeter.cajeta", IngestOptions{}, r);
    const Type* g = findType(r.model, "cajeta.lang.Greeter");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->kind, TypeKind::Class);
    ASSERT_FALSE(g->extends.empty());
    EXPECT_EQ(g->extends[0], "Object");

    const Member* greet = findMember(*g, "greet");
    ASSERT_NE(greet, nullptr);
    EXPECT_EQ(greet->kind, MemberKind::Method);
    EXPECT_TRUE(greet->returnTransfer); // #String
    EXPECT_EQ(greet->returnType, "String");
    ASSERT_EQ(greet->params.size(), 1u);
    EXPECT_EQ(greet->params[0].name, "name");
    EXPECT_EQ(greet->params[0].type, "String");
    ASSERT_TRUE(greet->doc != nullptr);
    EXPECT_FALSE(greet->doc->tags("Complexity").empty());
    EXPECT_FALSE(greet->doc->tags("FiberSafe").empty());

    const Member* ctor = findMember(*g, "Greeter");
    ASSERT_NE(ctor, nullptr);
    EXPECT_EQ(ctor->kind, MemberKind::Constructor);
}

TEST(Ingest, SignatureFidelityGenericsAndTransfer) {
    IngestResult r;
    ingestSource(
        "package p;\npublic class Box<T> {\n"
        "  /** add */ public void add(#T value) {}\n"
        "  /** get */ public #T get() {}\n}\n",
        "Box.cajeta", IngestOptions{}, r);
    const Type* b = findType(r.model, "p.Box");
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(b->typeParams.size(), 1u);
    EXPECT_EQ(b->typeParams[0].name, "T");
    const Member* add = findMember(*b, "add");
    ASSERT_NE(add, nullptr);
    ASSERT_EQ(add->params.size(), 1u);
    EXPECT_TRUE(add->params[0].ownershipTransfer);
    EXPECT_EQ(add->params[0].type, "T");
    const Member* get = findMember(*b, "get");
    ASSERT_NE(get, nullptr);
    EXPECT_TRUE(get->returnTransfer);
}

TEST(Ingest, VisibilityFilteringDefault) {
    IngestResult r;
    ingestSource(
        "package p;\npublic class C {\n  private void hidden() {}\n  public void shown() {}\n}\n",
        "C.cajeta", IngestOptions{}, r); // includePrivate=false
    const Type* c = findType(r.model, "p.C");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(findMember(*c, "hidden"), nullptr);
    EXPECT_NE(findMember(*c, "shown"), nullptr);
}

TEST(Ingest, EmptyInputSmoke) {
    IngestResult r = ingestTree(std::string(CAJETADOC_FIXTURES_DIR) + "/empty", IngestOptions{});
    EXPECT_GE(r.filesParsed, 1);
    const Type* blank = findType(r.model, "cajeta.blank.Blank");
    ASSERT_NE(blank, nullptr);
    EXPECT_EQ(blank->doc, nullptr); // no doc comment
}

TEST(Ingest, ModelJsonDeterministic) {
    auto build = []() {
        IngestResult r;
        ingestSource(slurp(fs::path(CAJETADOC_FIXTURES_DIR) / "lang" / "Greeter.cajeta"),
                     "lang/Greeter.cajeta", IngestOptions{}, r);
        return toJson(r.model);
    };
    EXPECT_EQ(build(), build()); // stable across runs
}

// ---------------------------------------------------------------------------
// §4 — Markdown rendering
// ---------------------------------------------------------------------------
TEST(Markdown, HeadingDemotion) {
    MarkdownOptions o;
    o.headingOffset = 2;
    std::string html = renderMarkdown("## Sub", o);
    EXPECT_NE(html.find("<h4>Sub</h4>"), std::string::npos);
}

// Regression: a line whose first non-space char is '#' but with no following
// space (e.g. a wrapped doc line `#Optional<R>`) is not an ATX heading. It must
// render as paragraph text, NOT spin the paragraph gatherer forever appending
// empty <p> (which previously ran cajetadoc out of memory on Tasks.cajeta).
TEST(Markdown, HashWithoutSpaceIsNotHeadingNoHang) {
    std::string html = renderMarkdown("intro line\n#Optional<R> tail\nmore\n");
    EXPECT_EQ(html.find("<h1"), std::string::npos);
    EXPECT_NE(html.find("#Optional&lt;R&gt;"), std::string::npos);
}

// Regression: 7+ '#' is past the heading ceiling and must also degrade to a
// paragraph rather than fall through into the same infinite loop.
TEST(Markdown, OverlongHashRunIsParagraphNoHang) {
    std::string html = renderMarkdown("####### too deep\n");
    EXPECT_EQ(html.find("<h"), std::string::npos);
    EXPECT_NE(html.find("####### too deep"), std::string::npos);
}

TEST(Markdown, InlineCodeAndStrong) {
    std::string html = renderInline("use `x` and **bold**");
    EXPECT_NE(html.find("<code>x</code>"), std::string::npos);
    EXPECT_NE(html.find("<strong>bold</strong>"), std::string::npos);
}

TEST(Markdown, FencedCodeBlockEscaped) {
    std::string html = renderMarkdown("```cajeta\na < b;\n```");
    EXPECT_NE(html.find("language-cajeta"), std::string::npos);
    EXPECT_NE(html.find("a &lt; b;"), std::string::npos);
}

TEST(Markdown, PipeTable) {
    std::string html = renderMarkdown("| A | B |\n|---|---|\n| 1 | 2 |\n");
    EXPECT_NE(html.find("<table>"), std::string::npos);
    EXPECT_NE(html.find("<th>A</th>"), std::string::npos);
    EXPECT_NE(html.find("<td>1</td>"), std::string::npos);
}

TEST(Markdown, UnorderedList) {
    std::string html = renderMarkdown("- one\n- two\n");
    EXPECT_NE(html.find("<ul>"), std::string::npos);
    EXPECT_NE(html.find("<li>one</li>"), std::string::npos);
}

TEST(Markdown, NoRawHtmlPassthrough) {
    std::string html = renderInline("<script>alert(1)</script>");
    EXPECT_EQ(html.find("<script>"), std::string::npos);
    EXPECT_NE(html.find("&lt;script&gt;"), std::string::npos);
}

TEST(Markdown, InlineCodeTagEscapes) {
    std::string html = renderInline("{@Code a < b}");
    EXPECT_NE(html.find("<code>a &lt; b</code>"), std::string::npos);
}

// ---------------------------------------------------------------------------
// §10/§11 — rendering & determinism
// ---------------------------------------------------------------------------
TEST(Render, TypePageContainsSignatureAndBadges) {
    IngestResult r;
    ingestSource(slurp(fs::path(CAJETADOC_FIXTURES_DIR) / "lang" / "Greeter.cajeta"),
                 "lang/Greeter.cajeta", IngestOptions{}, r);
    const Type* g = findType(r.model, "cajeta.lang.Greeter");
    ASSERT_NE(g, nullptr);
    std::string html = renderTypePage(*g, "cajetadoc.css");
    EXPECT_NE(html.find("class=\"cajetadoc\""), std::string::npos);
    EXPECT_NE(html.find("greet"), std::string::npos);
    EXPECT_NE(html.find("badge-fiber"), std::string::npos);   // FiberSafe badge
    EXPECT_NE(html.find("Parameters"), std::string::npos);    // @Param widget
    EXPECT_NE(html.find("Returns"), std::string::npos);       // @Return widget
}

TEST(Render, StylesheetIsZeroSpecificityAndLayered) {
    std::string css = defaultStylesheet();
    EXPECT_NE(css.find("@layer cajetadoc"), std::string::npos);
    EXPECT_NE(css.find(":where(.cajetadoc"), std::string::npos);
    EXPECT_NE(css.find("var(--cajetadoc-fg)"), std::string::npos) << "design token read";
    EXPECT_NE(css.find("var(--background,"), std::string::npos) << "host token fallthrough";
}

TEST(Render, SiteGenerationDeterministic) {
    IngestResult r;
    ingestSource(slurp(fs::path(CAJETADOC_FIXTURES_DIR) / "lang" / "Greeter.cajeta"),
                 "lang/Greeter.cajeta", IngestOptions{}, r);
    fs::path tmp = fs::temp_directory_path();
    fs::path a = tmp / "cajetadoc_a";
    fs::path b = tmp / "cajetadoc_b";
    fs::remove_all(a);
    fs::remove_all(b);
    std::string e1, e2;
    int n1 = generateSite(r.model, a.string(), e1);
    int n2 = generateSite(r.model, b.string(), e2);
    EXPECT_TRUE(e1.empty()) << e1;
    EXPECT_EQ(n1, n2);
    EXPECT_GT(n1, 0);
    // the per-type page must be byte-identical across runs
    std::string pa = slurp(a / "cajeta" / "lang" / "Greeter.html");
    std::string pb = slurp(b / "cajeta" / "lang" / "Greeter.html");
    EXPECT_FALSE(pa.empty());
    EXPECT_EQ(pa, pb);
    fs::remove_all(a);
    fs::remove_all(b);
}

// ---------------------------------------------------------------------------
// §1.1.1 — golden-file runner (inline markdown golden, byte-exact)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// §5 — cross-reference resolution
// ---------------------------------------------------------------------------
TEST(Resolve, FqnSimpleAndMember) {
    IngestResult r;
    ingestSource("package cajeta.lang;\npublic class Object { public int64 hash(); }\n",
                 "Object.cajeta", IngestOptions{}, r);
    ingestSource("package cajeta.hash;\n"
                 "public class Hash { public static int64 identity(Object obj); }\n",
                 "Hash.cajeta", IngestOptions{}, r);
    SymbolIndex idx;
    idx.build(r.model);

    auto fqn = idx.resolve("cajeta.lang.Object", "cajeta.hash");
    ASSERT_TRUE(fqn.has_value());
    EXPECT_EQ(fqn->page, "cajeta/lang/Object.html");
    EXPECT_EQ(fqn->anchor, "");

    auto simple = idx.resolve("Hash", "cajeta.hash");
    ASSERT_TRUE(simple.has_value());
    EXPECT_EQ(simple->page, "cajeta/hash/Hash.html");

    auto overload = idx.resolve("Hash.identity(pointer)", "cajeta.hash");
    ASSERT_TRUE(overload.has_value());
    EXPECT_EQ(overload->anchor, "identity"); // args stripped

    auto hashForm = idx.resolve("Object#hash", "cajeta.hash");
    ASSERT_TRUE(hashForm.has_value());
    EXPECT_EQ(hashForm->page, "cajeta/lang/Object.html");
    EXPECT_EQ(hashForm->anchor, "hash");

    EXPECT_FALSE(idx.resolve("Nonexistent", "cajeta.hash").has_value());
}

TEST(Resolve, HrefDepth) {
    SymbolIndex idx;
    ResolvedRef r{"cajeta/hash/Hash.html", "identity"};
    EXPECT_EQ(idx.href(r, 2), "../../cajeta/hash/Hash.html#identity");
    EXPECT_EQ(idx.href(r, 0), "cajeta/hash/Hash.html#identity");
}

TEST(Render, CrossReferenceLinksAndTypeLevelSee) {
    IngestResult r;
    ingestSource("package cajeta.lang;\npublic class Object {}\n", "Object.cajeta",
                 IngestOptions{}, r);
    ingestSource("package cajeta.hash;\n"
                 "/**\n * Uses `cajeta.lang.Object`.\n * @See cajeta.lang.Object\n"
                 " * @Since 0.1.0\n */\npublic class Hash {}\n",
                 "Hash.cajeta", IngestOptions{}, r);
    SymbolIndex idx;
    idx.build(r.model);
    const Type* h = findType(r.model, "cajeta.hash.Hash");
    ASSERT_NE(h, nullptr);
    std::string html = renderTypePage(*h, "cajetadoc.css", &idx);
    // backtick code span auto-linked to the resolved page
    EXPECT_NE(html.find("../../cajeta/lang/Object.html"), std::string::npos);
    // type-level @See and @Since now render (previously dropped)
    EXPECT_NE(html.find("See Also"), std::string::npos);
    EXPECT_NE(html.find("Since:"), std::string::npos);
}

TEST(Render, NavChromeBreadcrumbsAndSidebar) {
    IngestResult r;
    ingestSource("package cajeta.hash;\n/** A. */ public class Hash {}\n", "Hash.cajeta",
                 IngestOptions{}, r);
    ingestSource("package cajeta.hash;\n/** B. */ public class XXHash3 {}\n", "XXHash3.cajeta",
                 IngestOptions{}, r);
    SymbolIndex idx;
    idx.build(r.model);
    Package* pkg = r.model.findPackage("cajeta.hash");
    const Type* h = findType(r.model, "cajeta.hash.Hash");
    ASSERT_NE(pkg, nullptr);
    ASSERT_NE(h, nullptr);
    SiteMeta meta;
    meta.version = "0.5.1";
    meta.datePublished = "2026-06-05";
    meta.license = "Apache-2.0";
    std::string html = renderTypePage(*h, "../../cajetadoc.css", &idx, pkg, meta);
    // Fixed header bar holds brand and breadcrumbs (no pager).
    EXPECT_NE(html.find("class=\"topbar\""), std::string::npos);  // fixed header
    EXPECT_NE(html.find("class=\"brand\""), std::string::npos);   // icon + title
    EXPECT_NE(html.find("brand-title\">Cajeta"), std::string::npos);
    EXPECT_NE(html.find("class=\"crumbs\""), std::string::npos);  // breadcrumbs in header
    EXPECT_NE(html.find("v0.5.1"), std::string::npos);            // project meta
    EXPECT_NE(html.find("Apache-2.0"), std::string::npos);
    // Drill-down child navigation + pager live in the left side nav.
    EXPECT_NE(html.find("class=\"sidebar\""), std::string::npos);
    EXPECT_NE(html.find("class=\"pager\""), std::string::npos);
    EXPECT_NE(html.find("class=\"pager-ico\""), std::string::npos); // icon-only arrows
    EXPECT_NE(html.find("class=\"pkg-nav\""), std::string::npos);
    EXPECT_NE(html.find("XXHash3.html"), std::string::npos);        // sibling link + next target
    // Hash is the first type here, so the prev button renders but is disabled.
    EXPECT_NE(html.find("pager-btn prev disabled"), std::string::npos);
    EXPECT_NE(html.find("class=\"pager-btn next\""), std::string::npos);
    // The sidebar marks the current type (selection highlight).
    EXPECT_NE(html.find("class=\"active\""), std::string::npos);
}

TEST(Render, OverviewTypeCountFallback) {
    IngestResult r;
    ingestSource("package cajeta.hash;\npublic class Hash {}\n", "Hash.cajeta", IngestOptions{}, r);
    SymbolIndex idx;
    idx.build(r.model);
    std::string html = renderOverview(r.model, "cajetadoc.css", &idx);
    EXPECT_NE(html.find("1 type"), std::string::npos); // blank summary -> type count
}

TEST(Ingest, PackageLevelDoc) {
    IngestResult r;
    ingestSource("/** The hashing package. */\npackage cajeta.hash;\npublic class Hash {}\n",
                 "package.cajeta", IngestOptions{}, r);
    Package* p = r.model.findPackage("cajeta.hash");
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(p->doc != nullptr);
    EXPECT_EQ(p->doc->summary, "The hashing package.");
}

TEST(Golden, InlineMarkdownSnippet) {
    std::string produced = renderMarkdown(
        "Hello **world**.\n\n- a\n- b\n\n`code` and a [link](http://x).\n");
    expectGolden(produced, fs::path(CAJETADOC_FIXTURES_DIR).parent_path() / "golden" /
                               "inline_snippet.html");
}

// ---- @EntryPoint (docs-refactor plan 3.1.1, 3.1.2) ----------------------

namespace {
const char* kEntrySrc =
    "package cajeta.demo;\n"
    "public class Api {\n"
    "    /**\n"
    "     * Front door.\n"
    "     *\n"
    "     * @EntryPoint\n"
    "     */\n"
    "    public static void open() {}\n"
    "    /** Internal helper. */\n"
    "    public static void helperOnly() {}\n"
    "}\n";
} // namespace

// 3.1.1 — tag parses, reaches the model JSON, and renders a badge.
TEST(EntryPoint, TagParsesEmitsJsonAndBadge) {
    IngestResult r;
    ingestSource(kEntrySrc, "Api.cajeta", IngestOptions{}, r);
    const Type* api = findType(r.model, "cajeta.demo.Api");
    ASSERT_NE(api, nullptr);
    const Member* open = findMember(*api, "open");
    ASSERT_NE(open, nullptr);
    ASSERT_TRUE(open->doc != nullptr);
    EXPECT_FALSE(open->doc->tags("EntryPoint").empty());

    std::string json = toJson(r.model);
    EXPECT_NE(json.find("\"name\": \"EntryPoint\""), std::string::npos);

    const Package* pkg = r.model.findPackage("cajeta.demo");
    std::string html = renderTypePage(*api, "cajetadoc.css", nullptr, pkg);
    EXPECT_NE(html.find("badge-entry"), std::string::npos);
}

// 3.1.2 — site-wide entry-points index aggregates tagged methods per
// package; untagged members stay out.
TEST(EntryPoint, IndexPageAggregatesPerPackage) {
    IngestResult r;
    ingestSource(kEntrySrc, "Api.cajeta", IngestOptions{}, r);
    ingestSource(
        "package cajeta.other;\n"
        "public class Tool {\n"
        "    /**\n"
        "     * Start here.\n"
        "     *\n"
        "     * @EntryPoint\n"
        "     */\n"
        "    public static void begin() {}\n"
        "}\n",
        "Tool.cajeta", IngestOptions{}, r);

    std::string html = renderEntryPointsIndex(r.model, "cajetadoc.css");
    EXPECT_NE(html.find("cajeta.demo"), std::string::npos);
    EXPECT_NE(html.find("open"), std::string::npos);
    EXPECT_NE(html.find("cajeta.other"), std::string::npos);
    EXPECT_NE(html.find("begin"), std::string::npos);
    EXPECT_EQ(html.find("helperOnly"), std::string::npos);

    // generateSite writes it at the site root.
    fs::path out = fs::temp_directory_path() / "cajetadoc-entry-test";
    fs::remove_all(out);
    std::string err;
    SiteMeta meta;
    int pages = generateSite(r.model, out.string(), err, meta);
    ASSERT_GT(pages, 0) << err;
    EXPECT_TRUE(fs::exists(out / "entry-points.html"));
    fs::remove_all(out);
}
