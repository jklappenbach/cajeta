// Tests for the front-matter document splitter.
// See src/cajeta/buildtool/FrontMatter.h and
// docs/specs/yaml-frontmatter-spec.md §2 (plan unit D.Y1).

#include "cajeta/buildtool/FrontMatter.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

using cajeta::buildtool::FrontMatterSplit;
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
