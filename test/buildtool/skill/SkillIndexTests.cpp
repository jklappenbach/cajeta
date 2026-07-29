// Tests for the per-package skill index.
// See src/cajeta/buildtool/skill/SkillIndex.h and
// specs/archive/skill-discovery-spec.md §2.3, §3 (plan unit D.2).

#include "cajeta/buildtool/skill/SkillIndex.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <set>
#include <string>

using cajeta::buildtool::skill::SkillCandidate;
using cajeta::buildtool::skill::SkillDocument;
using cajeta::buildtool::skill::SkillIndex;

namespace {

    SkillDocument doc(std::string id, std::vector<std::string> appliesTo,
                      std::string title = "") {
        SkillDocument d;
        d.id = std::move(id);
        d.appliesTo = std::move(appliesTo);
        d.title = std::move(title);
        d.body = "body";
        return d;
    }

    template <class T> T unwrap(llvm::Expected<T> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return T{};
        }
        return std::move(*e);
    }

    std::set<std::string> idSet(const std::vector<std::string>& v) {
        return {v.begin(), v.end()};
    }

    std::set<std::string> candidateKeys(const std::vector<SkillCandidate>& v) {
        std::set<std::string> out;
        for (const auto& c : v) out.insert(c.key);
        return out;
    }

} // namespace

// D.2.1 — query resolves a canonical name to its bound ids.
TEST(SkillIndexTests, queryExact) {
    auto idx = unwrap(SkillIndex::build(
        {doc("file-open", {"cajeta/io/File", "cajeta/io/File.open"})}));
    EXPECT_EQ(idSet(idx.query("cajeta/io/File", false)),
              (std::set<std::string>{"file-open"}));
    EXPECT_EQ(idSet(idx.query("cajeta/io/File.open", false)),
              (std::set<std::string>{"file-open"}));
    EXPECT_TRUE(idx.query("cajeta/io/Nope", false).empty());
}

// D.2.1 / §3.2 — hierarchical query returns a name's descendants.
TEST(SkillIndexTests, queryHierarchicalDescendants) {
    auto idx = unwrap(SkillIndex::build({
        doc("nn-overview", {"cajeta/torch/nn"}),
        doc("linear", {"cajeta/torch/nn/Linear"}),
        doc("forward", {"cajeta/torch/nn/Linear.forward"}),
        doc("other", {"cajeta/io/File"}),
    }));
    EXPECT_EQ(idSet(idx.query("cajeta/torch/nn", false)),
              (std::set<std::string>{"nn-overview"}));
    EXPECT_EQ(idSet(idx.query("cajeta/torch/nn", true)),
              (std::set<std::string>{"nn-overview", "linear", "forward"}));
    EXPECT_EQ(idSet(idx.query("cajeta/torch/nn/Linear", true)),
              (std::set<std::string>{"linear", "forward"}));
}

// D.2.1 — build → serialize → deserialize round-trips (deterministic).
TEST(SkillIndexTests, serializeDeserializeRoundTrips) {
    auto idx = unwrap(SkillIndex::build({
        doc("file-open", {"cajeta/io/File"}, "Opening files"),
        doc("nn", {"cajeta/torch/nn"}, "Neural nets"),
    }));
    std::string s1 = idx.serialize();
    EXPECT_NE(s1.find("skill-index-v1"), std::string::npos);

    auto idx2 = unwrap(SkillIndex::deserialize(s1));
    EXPECT_EQ(idx2.serialize(), s1); // stable
    EXPECT_EQ(idSet(idx2.query("cajeta/io/File", false)),
              (std::set<std::string>{"file-open"}));
    ASSERT_NE(idx2.entry("file-open"), nullptr);
    EXPECT_EQ(idx2.entry("file-open")->member, "skills/file-open.md");
    EXPECT_EQ(idx2.entry("file-open")->title, "Opening files");
}

// D.2.1 — trigram candidates cover both names and titles for near-miss queries.
// The prefilter is recall-oriented (a superset the D.4a matcher then scores), so
// we assert it *includes* the right key and *filters* when nothing shares a
// trigram — not that it excludes keys sharing a common prefix.
TEST(SkillIndexTests, trigramCandidatesNamesAndTitles) {
    auto idx = unwrap(SkillIndex::build({
        doc("file-open", {"cajeta/io/File"}, "Opening files"),
        doc("socket", {"cajeta/net/Socket"}, "Network sockets"),
    }));
    using cajeta::buildtool::skill::MatchSource;

    // Near-miss on a NAME surfaces that name, tagged as a Name key.
    auto nameCands = idx.candidates("cajeta/io/Fille");
    auto byName = candidateKeys(nameCands);
    EXPECT_TRUE(byName.count("cajeta/io/File"));
    for (const auto& c : nameCands) {
        if (c.key == "cajeta/io/File") {
            EXPECT_EQ(c.source, MatchSource::Name);
            EXPECT_EQ(idSet(c.ids), (std::set<std::string>{"file-open"}));
        }
    }

    // Near-miss on a TITLE surfaces that title, tagged as a Title key.
    auto titleCands = idx.candidates("Openin files");
    auto byTitle = candidateKeys(titleCands);
    EXPECT_TRUE(byTitle.count("Opening files"));
    for (const auto& c : titleCands) {
        if (c.key == "Opening files") {
            EXPECT_EQ(c.source, MatchSource::Title);
            EXPECT_EQ(idSet(c.ids), (std::set<std::string>{"file-open"}));
        }
    }

    // A query sharing no trigram with any key is filtered out entirely.
    EXPECT_TRUE(idx.candidates("zzqqww").empty());
}

// D.2.1 — duplicate skill id is rejected.
TEST(SkillIndexTests, duplicateIdRejected) {
    auto e = SkillIndex::build({
        doc("dup", {"cajeta/io/File"}),
        doc("dup", {"cajeta/io/Path"}),
    });
    ASSERT_FALSE((bool)e);
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << e.takeError();
    EXPECT_NE(msg.find("dup"), std::string::npos);
}

// D.2.3 — an unknown schema version is rejected, naming the supported one.
TEST(SkillIndexTests, unknownSchemaVersionRejected) {
    auto e = SkillIndex::deserialize(
        "{\"schema-version\": \"skill-index-v2\", \"names\": {}, \"skills\": {}}");
    ASSERT_FALSE((bool)e);
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << e.takeError();
    EXPECT_NE(msg.find("skill-index-v1"), std::string::npos);
}
