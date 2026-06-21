// Tests for the skill document model + parser.
// See src/cajeta/buildtool/skill/SkillDocument.h and
// docs/specs/skill-discovery-spec.md §4 (plan unit D.1).

#include "cajeta/buildtool/skill/SkillDocument.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

using cajeta::buildtool::skill::SkillDocument;

namespace {

    SkillDocument unwrap(llvm::Expected<SkillDocument> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return {};
        }
        return std::move(*e);
    }

    std::string errorText(llvm::Expected<SkillDocument>&& e) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << e.takeError();
        return out;
    }

    // The §4.1 example document.
    const char* kValid =
        "---\n"
        "id: file-open\n"
        "applies-to: [cajeta/io/File, cajeta/io/File.open]\n"
        "title: Opening files\n"
        "description: How to open and dispose File handles correctly.\n"
        "---\n"
        "# Opening files\n\nUse `File.open` and dispose deterministically.\n";

} // namespace

// uc 5.1 / §4.1 — a valid skill parses into model + verbatim body.
TEST(SkillDocumentTests, parsesValidSkill) {
    auto d = unwrap(SkillDocument::parse(kValid, "skills/file-open.md"));
    EXPECT_EQ(d.id, "file-open");
    ASSERT_EQ(d.appliesTo.size(), 2u);
    EXPECT_EQ(d.appliesTo[0], "cajeta/io/File");
    EXPECT_EQ(d.appliesTo[1], "cajeta/io/File.open");
    EXPECT_EQ(d.title, "Opening files");
    EXPECT_EQ(d.description, "How to open and dispose File handles correctly.");
    EXPECT_EQ(d.body, "# Opening files\n\nUse `File.open` and dispose deterministically.\n");
}

// §4.1 — title/description are optional.
TEST(SkillDocumentTests, titleAndDescriptionOptional) {
    auto d = unwrap(SkillDocument::parse(
        "---\nid: x\napplies-to: [cajeta/io/File]\n---\nbody\n"));
    EXPECT_EQ(d.id, "x");
    EXPECT_TRUE(d.title.empty());
    EXPECT_TRUE(d.description.empty());
}

// D.1.1 — `applies-to` accepts each of the four name kinds (§3.1).
TEST(SkillDocumentTests, acceptsFourNameKinds) {
    auto d = unwrap(SkillDocument::parse(
        "---\n"
        "id: kinds\n"
        "applies-to: [cajeta.io, cajeta/torch/nn, cajeta/torch/nn/Linear, "
        "cajeta/torch/nn/Linear.forward]\n"
        "---\nbody\n"));
    ASSERT_EQ(d.appliesTo.size(), 4u);
    EXPECT_EQ(d.appliesTo[0], "cajeta.io");
    EXPECT_EQ(d.appliesTo[3], "cajeta/torch/nn/Linear.forward");
}

// D.1.1 — missing required `id` is rejected with field context.
TEST(SkillDocumentTests, missingIdRejected) {
    auto e = SkillDocument::parse(
        "---\napplies-to: [cajeta/io/File]\n---\nbody\n", "skills/bad.md");
    ASSERT_FALSE((bool)e);
    std::string msg = errorText(std::move(e));
    EXPECT_NE(msg.find("id"), std::string::npos);
    EXPECT_NE(msg.find("bad.md"), std::string::npos);
}

// D.1.1 — missing required `applies-to` is rejected.
TEST(SkillDocumentTests, missingAppliesToRejected) {
    auto e = SkillDocument::parse("---\nid: x\n---\nbody\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("applies-to"), std::string::npos);
}

// D.1.1 — empty `applies-to` list is rejected.
TEST(SkillDocumentTests, emptyAppliesToRejected) {
    auto e = SkillDocument::parse("---\nid: x\napplies-to: []\n---\nbody\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("applies-to"), std::string::npos);
}

// D.1.1 — an empty entry inside `applies-to` is rejected.
TEST(SkillDocumentTests, emptyAppliesToEntryRejected) {
    auto e = SkillDocument::parse(
        "---\nid: x\napplies-to: [cajeta/io/File, '']\n---\nbody\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("applies-to"), std::string::npos);
}

// D.1.1 — malformed YAML frontmatter is rejected.
TEST(SkillDocumentTests, malformedYamlRejected) {
    auto e = SkillDocument::parse("---\nid: 'oops\napplies-to: [a]\n---\nbody\n");
    ASSERT_FALSE((bool)e);
    // parse error from the YAML facility (carries a line number).
    EXPECT_NE(errorText(std::move(e)).find("line"), std::string::npos);
}

// D.1.1 — a document with no frontmatter block is rejected.
TEST(SkillDocumentTests, missingFrontmatterRejected) {
    auto e = SkillDocument::parse("# Just markdown\nno header\n", "skills/plain.md");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("frontmatter"), std::string::npos);
}
