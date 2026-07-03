// Tests for the YAML-header parser (frontmatter subset).
// See src/cajeta/buildtool/Yaml.h and
// specs/archive/yaml-frontmatter-spec.md §3 (plan unit D.Y2 — scalars + mappings).

#include "cajeta/buildtool/Yaml.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

using cajeta::buildtool::parseYaml;

namespace {

    llvm::json::Value unwrap(llvm::Expected<llvm::json::Value> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return nullptr;
        }
        return std::move(*e);
    }

    std::string errorText(llvm::Expected<llvm::json::Value>&& e) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << e.takeError();
        return out;
    }

} // namespace

// uc 3.3.1 — `key: value` pairs parse to a JSON object with the right scalar types.
TEST(YamlTests, scalarTypingAndMap) {
    auto v = unwrap(parseYaml(
        "yes: true\nno: false\nn1: null\nn2: ~\ni: 42\nneg: -7\nx: 3.14\ns: hello\n"));
    auto* o = v.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getBoolean("yes"), std::optional<bool>(true));
    EXPECT_EQ(o->getBoolean("no"), std::optional<bool>(false));
    ASSERT_TRUE(o->get("n1"));
    EXPECT_EQ(o->get("n1")->kind(), llvm::json::Value::Null);
    ASSERT_TRUE(o->get("n2"));
    EXPECT_EQ(o->get("n2")->kind(), llvm::json::Value::Null);
    EXPECT_EQ(o->getInteger("i"), std::optional<int64_t>(42));
    EXPECT_EQ(o->getInteger("neg"), std::optional<int64_t>(-7));
    EXPECT_EQ(o->getNumber("x"), std::optional<double>(3.14));
    EXPECT_EQ(o->getString("s"), std::optional<llvm::StringRef>("hello"));
}

// §3.1 — a quoted scalar is always a string, even when it looks numeric/bool/null.
TEST(YamlTests, quotedScalarsAreAlwaysStrings) {
    auto v = unwrap(parseYaml("a: 'true'\nb: \"42\"\nc: '~'\nd: \"a\\nb\"\n"));
    auto* o = v.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getString("a"), std::optional<llvm::StringRef>("true"));
    EXPECT_EQ(o->getString("b"), std::optional<llvm::StringRef>("42"));
    EXPECT_EQ(o->getString("c"), std::optional<llvm::StringRef>("~"));
    EXPECT_EQ(o->getString("d"), std::optional<llvm::StringRef>("a\nb"));
}

// uc 3.3.3 — comments and blank lines are ignored; trailing inline comment dropped.
TEST(YamlTests, commentsAndBlankLinesIgnored) {
    auto v = unwrap(parseYaml(
        "# leading comment\n\nk: v   # inline comment\nj: 7\nurl: http://x#frag\n"));
    auto* o = v.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getString("k"), std::optional<llvm::StringRef>("v"));
    EXPECT_EQ(o->getInteger("j"), std::optional<int64_t>(7));
    // '#' not preceded by whitespace is literal, not a comment.
    EXPECT_EQ(o->getString("url"), std::optional<llvm::StringRef>("http://x#frag"));
}

// §3.1 — nested mapping by indentation → nested JSON object.
TEST(YamlTests, nestedMapping) {
    auto v = unwrap(parseYaml("outer:\n  inner: 1\n  name: bob\ntop: 9\n"));
    auto* o = v.getAsObject();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->getInteger("top"), std::optional<int64_t>(9));
    auto* inner = o->getObject("outer");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->getInteger("inner"), std::optional<int64_t>(1));
    EXPECT_EQ(inner->getString("name"), std::optional<llvm::StringRef>("bob"));
}

// §3.1 — a key with empty value and no indented child is null.
TEST(YamlTests, emptyValueIsNull) {
    auto v = unwrap(parseYaml("k:\n"));
    auto* o = v.getAsObject();
    ASSERT_NE(o, nullptr);
    ASSERT_TRUE(o->get("k"));
    EXPECT_EQ(o->get("k")->kind(), llvm::json::Value::Null);
}

// uc 3.3.4 — unterminated quote → error naming the line.
TEST(YamlTests, unterminatedQuoteIsError) {
    auto e = parseYaml("ok: 1\nbad: 'oops\n");
    ASSERT_FALSE((bool)e);
    std::string msg = errorText(std::move(e));
    EXPECT_NE(msg.find("line 2"), std::string::npos);
}

// uc 3.3.4 — tab indentation → error naming the line.
TEST(YamlTests, tabIndentationIsError) {
    auto e = parseYaml("outer:\n\tinner: 1\n");
    ASSERT_FALSE((bool)e);
    std::string msg = errorText(std::move(e));
    EXPECT_NE(msg.find("line 2"), std::string::npos);
    EXPECT_NE(msg.find("tab"), std::string::npos);
}

// --- D.Y3: sequences (block + flow) + nesting (spec §3.1; uc 3.3.2) ---

// uc 3.3.2 — flow sequence of plain scalars → JSON array of strings.
TEST(YamlTests, flowSequenceOfStrings) {
    auto v = unwrap(parseYaml("applies-to: [a, b, c]\n"));
    auto* a = v.getAsObject()->getArray("applies-to");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->size(), 3u);
    EXPECT_EQ((*a)[0].getAsString(), std::optional<llvm::StringRef>("a"));
    EXPECT_EQ((*a)[2].getAsString(), std::optional<llvm::StringRef>("c"));
}

// §3.1 — empty flow sequence → empty array.
TEST(YamlTests, flowSequenceEmpty) {
    auto v = unwrap(parseYaml("x: []\n"));
    auto* a = v.getAsObject()->getArray("x");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->size(), 0u);
}

// §3.1 — flow items carry scalar typing and quoting.
TEST(YamlTests, flowSequenceTypedAndQuoted) {
    auto v = unwrap(parseYaml("n: [1, 2, 3]\nq: ['1', true, \"x\"]\n"));
    auto* o = v.getAsObject();
    auto* n = o->getArray("n");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ((*n)[0].getAsInteger(), std::optional<int64_t>(1));
    EXPECT_EQ((*n)[2].getAsInteger(), std::optional<int64_t>(3));
    auto* q = o->getArray("q");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ((*q)[0].getAsString(), std::optional<llvm::StringRef>("1")); // quoted → string
    EXPECT_EQ((*q)[1].getAsBoolean(), std::optional<bool>(true));
    EXPECT_EQ((*q)[2].getAsString(), std::optional<llvm::StringRef>("x"));
}

// uc 3.3.2 — block sequence (`- ` lines under a key) → JSON array.
TEST(YamlTests, blockSequence) {
    auto v = unwrap(parseYaml("items:\n  - a\n  - b\n  - 3\nafter: ok\n"));
    auto* o = v.getAsObject();
    auto* items = o->getArray("items");
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size(), 3u);
    EXPECT_EQ((*items)[0].getAsString(), std::optional<llvm::StringRef>("a"));
    EXPECT_EQ((*items)[2].getAsInteger(), std::optional<int64_t>(3));
    EXPECT_EQ(o->getString("after"), std::optional<llvm::StringRef>("ok"));
}

// D.Y3.3 — block and flow forms produce identical JSON arrays.
TEST(YamlTests, blockAndFlowAreEquivalent) {
    auto flow = unwrap(parseYaml("applies-to: [a, b]\n"));
    auto block = unwrap(parseYaml("applies-to:\n  - a\n  - b\n"));
    EXPECT_EQ(flow, block);
}

// §3.1 — nesting: a block sequence of flow sequences.
TEST(YamlTests, nestedSequences) {
    auto v = unwrap(parseYaml("matrix:\n  - [1, 2]\n  - [3, 4]\n"));
    auto* m = v.getAsObject()->getArray("matrix");
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->size(), 2u);
    auto* row1 = (*m)[1].getAsArray();
    ASSERT_NE(row1, nullptr);
    EXPECT_EQ((*row1)[0].getAsInteger(), std::optional<int64_t>(3));
    EXPECT_EQ((*row1)[1].getAsInteger(), std::optional<int64_t>(4));
}

// uc 3.3.4 — malformed flow (unclosed `[`) → error naming the line.
TEST(YamlTests, unclosedFlowSequenceIsError) {
    auto e = parseYaml("ok: 1\nbad: [a, b\n");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(errorText(std::move(e)).find("line 2"), std::string::npos);
}
