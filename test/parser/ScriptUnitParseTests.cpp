//
// script-units U1 (spec 2.1-2.6) — grammar: script-shaped compilation units.
//
// A compilation unit may be a sequence of scriptMembers — loose block
// statements and top-level method declarations freely interleaved with type
// declarations — in addition to the ordinary package/imports/types shape.
// These tests drive the lexer/parser directly (no Compiler, no JIT) and
// assert on syntax-error counts plus the parse-tree shape via toStringTree,
// so they compile against the generated parser both before and after the
// grammar change: before it, every script-shaped case fails with syntax
// errors — the TDD "fails for the right reason" state.
//
// Semantics (implicit class synthesis, session bindings, the break-outside-
// a-loop diagnosis of spec 2.6) land in U2+ — nothing here executes.
//

#include "gtest/gtest.h"

#include "CajetaLexer.h"
#include "CajetaParser.h"

#include <string>

using cajeta::CajetaLexer;
using cajeta::CajetaParser;

namespace {

struct ParsedUnit {
    antlr4::ANTLRInputStream input;
    CajetaLexer lexer;
    antlr4::CommonTokenStream tokens;
    CajetaParser parser;
    CajetaParser::CompilationUnitContext* ctx = nullptr;

    explicit ParsedUnit(const std::string& src)
        : input(src), lexer(&input), tokens(&lexer), parser(&tokens) {
        ctx = parser.compilationUnit();
    }

    size_t syntaxErrors() { return parser.getNumberOfSyntaxErrors(); }
    std::string tree() { return ctx->toStringTree(&parser); }
    bool isScriptShape() {
        return tree().find("scriptMember") != std::string::npos;
    }
};

}  // namespace

// 1.1.1 / spec 2.1 — imports followed by loose statements parse as a script
// unit with zero syntax errors.
TEST(ScriptUnitParseTests, looseStatementsParse) {
    ParsedUnit p(
        "import cajeta.collection.ArrayList;\n"
        "var xs = heap ArrayList<int32>();\n"
        "xs.add(1);\n"
        "int32 n = 2;\n");
    EXPECT_EQ(0u, p.syntaxErrors());
    EXPECT_TRUE(p.isScriptShape());
}

// 1.1.2 / spec 2.2 — a class declaration, a top-level method, and loose
// statements coexist in one unit.
TEST(ScriptUnitParseTests, mixedMembersParse) {
    ParsedUnit p(
        "public class Point {\n"
        "    public int32 x;\n"
        "    public Point(int32 x) { this.x = x; }\n"
        "}\n"
        "int32 twice(int32 v) { return v * 2; }\n"
        "Point p = heap Point(21);\n"
        "int32 r = twice(p.x);\n");
    EXPECT_EQ(0u, p.syntaxErrors());
    EXPECT_TRUE(p.isScriptShape());
    // The class rides through as a full typeDeclaration inside the script
    // shape (spec 3.3: a normal top-level type).
    EXPECT_NE(std::string::npos, p.tree().find("classDeclaration"));
}

// 1.1.3 / spec 2.4 — an ordinary unit still parses cleanly and does NOT take
// the script alternative: no scriptMember node appears in its tree, and its
// typeDeclaration list is intact.
TEST(ScriptUnitParseTests, ordinaryUnitsUnchanged) {
    ParsedUnit p(
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class App {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n");
    EXPECT_EQ(0u, p.syntaxErrors());
    EXPECT_FALSE(p.isScriptShape());
    ASSERT_NE(nullptr, p.ctx);
    EXPECT_EQ(1u, p.ctx->typeDeclaration().size());
}

// Grammar-change regression: the empty unit (EOF only) stays legal.
TEST(ScriptUnitParseTests, emptyUnitStillParses) {
    ParsedUnit p("");
    EXPECT_EQ(0u, p.syntaxErrors());
}

// 1.1.4 / spec 2.5 — the package declaration is optional on a script unit,
// and honored when present.
TEST(ScriptUnitParseTests, packageOptional) {
    ParsedUnit bare(
        "int32 a = 1;\n"
        "a = a + 1;\n");
    EXPECT_EQ(0u, bare.syntaxErrors());
    EXPECT_TRUE(bare.isScriptShape());

    ParsedUnit packaged(
        "package tools.demo;\n"
        "int32 a = 1;\n");
    EXPECT_EQ(0u, packaged.syntaxErrors());
    EXPECT_TRUE(packaged.isScriptShape());
    EXPECT_NE(std::string::npos, packaged.tree().find("packageDeclaration"));
}

// 1.1.5 / spec 2.6 (parse half) — method-body-only constructs like `break;`
// PARSE at top level (they are blockStatements); their rejection is the same
// semantic diagnosis a method body gets, and lands with the U2 synthesis
// (the statement ends up inside the synthesized entry body where the
// existing break-outside-a-loop check fires).
TEST(ScriptUnitParseTests, topLevelBreakParsesForSemanticRejection) {
    ParsedUnit p("break;\n");
    EXPECT_EQ(0u, p.syntaxErrors());
    EXPECT_TRUE(p.isScriptShape());
}

// A trailing expression statement is a legal unit tail (spec 5.6's shape —
// its unit-result meaning arrives with the hosts).
TEST(ScriptUnitParseTests, trailingExpressionParses) {
    ParsedUnit p(
        "int32 a = 20;\n"
        "int32 b = 22;\n"
        "a + b;\n");
    EXPECT_EQ(0u, p.syntaxErrors());
    EXPECT_TRUE(p.isScriptShape());
}
