// Tests for the YAML-header parser (frontmatter subset).
// See src/cajeta/buildtool/Yaml.h and
// docs/specs/yaml-frontmatter-spec.md §3 (plan unit D.Y2 — scalars + mappings).

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
