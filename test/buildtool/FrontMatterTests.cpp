// Tests for the front-matter document splitter.
// See src/cajeta/buildtool/FrontMatter.h and
// docs/specs/yaml-frontmatter-spec.md §2 (plan unit D.Y1).

#include "cajeta/buildtool/FrontMatter.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

using cajeta::buildtool::FrontMatter;
using cajeta::buildtool::FrontMatterSplit;
using cajeta::buildtool::parseFrontMatter;
using cajeta::buildtool::parseFrontMatterFile;
using cajeta::buildtool::splitFrontMatter;

namespace {

    FrontMatterSplit unwrap(llvm::Expected<FrontMatterSplit> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return {};
        }
        return std::move(*e);
    }

    std::string errorText(llvm::Expected<FrontMatterSplit>&& e) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << e.takeError();
        return out;
    }

} // namespace

// uc 2.2.1 — header/body split at the closing fence.
TEST(FrontMatterSplitTests, splitsHeaderAndBody) {
    auto r = unwrap(splitFrontMatter("---\nid: x\n---\n# Body\ntext\n"));
    EXPECT_TRUE(r.present);
    EXPECT_EQ(r.header, "id: x\n");
    EXPECT_EQ(r.body, "# Body\ntext\n");
}

// uc 2.2.2 — no fence: whole input is the body, byte-for-byte.
TEST(FrontMatterSplitTests, noFenceWholeInputIsBody) {
    std::string src = "# Just markdown\nno frontmatter\n";
    auto r = unwrap(splitFrontMatter(src));
    EXPECT_FALSE(r.present);
    EXPECT_TRUE(r.header.empty());
    EXPECT_EQ(r.body, src);
}

// uc 2.2.3 — opening fence with no closing fence is an error.
TEST(FrontMatterSplitTests, unterminatedFenceIsError) {
    auto e = splitFrontMatter("---\nid: x\nno closing fence\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("closing fence"), std::string::npos);
}

// §2.1 — CRLF endings + leading UTF-8 BOM; body bytes after the fence preserved.
TEST(FrontMatterSplitTests, crlfAndBomHandled) {
    auto r = unwrap(splitFrontMatter("\xEF\xBB\xBF---\r\nid: x\r\n---\r\nbody\r\n"));
    EXPECT_TRUE(r.present);
    EXPECT_EQ(r.header, "id: x\r\n");
    EXPECT_EQ(r.body, "body\r\n");
}

// §2.1 — `...` is accepted as a closing fence terminator.
TEST(FrontMatterSplitTests, dotsCloseFence) {
    auto r = unwrap(splitFrontMatter("---\nid: x\n...\nbody\n"));
    EXPECT_TRUE(r.present);
    EXPECT_EQ(r.header, "id: x\n");
    EXPECT_EQ(r.body, "body\n");
}

// §2.1 — closing fence at EOF (no trailing newline) yields an empty body.
TEST(FrontMatterSplitTests, closingFenceAtEofEmptyBody) {
    auto r = unwrap(splitFrontMatter("---\nid: x\n---\n"));
    EXPECT_TRUE(r.present);
    EXPECT_EQ(r.body, "");
}

// --- D.Y4: public parseFrontMatter facility (spec §4; uc 4.2.1–4.2.2) ---

namespace {

    FrontMatter unwrapFm(llvm::Expected<FrontMatter> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return {};
        }
        return std::move(*e);
    }

    std::string fmErrorText(llvm::Expected<FrontMatter>&& e) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << e.takeError();
        return out;
    }

} // namespace

// uc 4.2.1 — a skill-shaped doc parses to metadata Value + verbatim body.
TEST(FrontMatterParseTests, parsesSkillShapedDoc) {
    std::string src =
        "---\n"
        "id: borrow-checker\n"
        "applies-to: [cajeta.mem, cajeta.borrow]\n"
        "title: \"Borrow checking\"\n"
        "description: how to satisfy the checker\n"
        "---\n"
        "# Borrow checking\n\nBody text.\n";
    auto fm = unwrapFm(parseFrontMatter(src));
    auto* o = fm.frontmatter.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getString("id"), std::optional<llvm::StringRef>("borrow-checker"));
    EXPECT_EQ(o->getString("title"), std::optional<llvm::StringRef>("Borrow checking"));
    auto* applies = o->getArray("applies-to");
    ASSERT_NE(applies, nullptr);
    ASSERT_EQ(applies->size(), 2u);
    EXPECT_EQ((*applies)[0].getAsString(), std::optional<llvm::StringRef>("cajeta.mem"));
    EXPECT_EQ(fm.body, "# Borrow checking\n\nBody text.\n");
}

// uc 4.2.1 / §2.2.2 — a plain .md doc yields {} and the whole input as body.
TEST(FrontMatterParseTests, noFrontmatterIsEmptyObjectWholeBody) {
    std::string src = "# Just markdown\nno header\n";
    auto fm = unwrapFm(parseFrontMatter(src));
    auto* o = fm.frontmatter.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->size(), 0u);
    EXPECT_EQ(fm.body, src);
}

// §4.1 — determinism: same input → identical value and body across parses.
TEST(FrontMatterParseTests, deterministic) {
    std::string src = "---\nid: x\nn: [1, 2]\n---\nbody\n";
    auto a = unwrapFm(parseFrontMatter(src));
    auto b = unwrapFm(parseFrontMatter(src));
    EXPECT_EQ(a.frontmatter, b.frontmatter);
    EXPECT_EQ(a.body, b.body);
}

// uc 4.2.2 — a parse error reports the document-absolute line.
TEST(FrontMatterParseTests, parseErrorHasDocumentLine) {
    // The unterminated quote is on document line 3 (fence is line 1).
    auto e = parseFrontMatter("---\nid: x\nbad: 'oops\n---\nbody\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(fmErrorText(std::move(e)).find("line 3"), std::string::npos);
}

// uc 4.2.1 — file variant reads disk and returns the parsed document.
TEST(FrontMatterParseTests, fileVariantReadsDisk) {
    llvm::SmallString<128> path;
    int fd = 0;
    ASSERT_FALSE((bool)llvm::sys::fs::createTemporaryFile("fm", "md", fd, path));
    {
        llvm::raw_fd_ostream os(fd, /*shouldClose=*/true);
        os << "---\nid: ondisk\n---\nbody\n";
    }
    auto fm = unwrapFm(parseFrontMatterFile(path));
    auto* o = fm.frontmatter.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getString("id"), std::optional<llvm::StringRef>("ondisk"));
    EXPECT_EQ(fm.body, "body\n");
    llvm::sys::fs::remove(path);
}

// uc 4.2.2 — a missing file is an error carrying the path, not a crash.
TEST(FrontMatterParseTests, missingFileErrorCarriesPath) {
    auto e = parseFrontMatterFile("/no/such/cajeta/skill-zzz.md");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(fmErrorText(std::move(e)).find("skill-zzz.md"), std::string::npos);
}
