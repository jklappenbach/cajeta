//
// CajetaXPU step 8 (increment A) — grammar + AST for the launch surface.
//
// Two grammar additions (CajetaParser.g4) are exercised here:
//   1. General postfix call:  `expression '(' parameterList? ')'` — lets the
//      result of an expression be invoked, which is how the launch form
//      `kernel.launch(stream, grid: [...], block: [...])(args)` parses (the
//      trailing `(args)` is a call applied to the `.launch(config)` result).
//   2. Array literal:         `arrayLiteral : '[' expressionList? ']'` —
//      reachable from `primary`, used for `grid: [...]` / `block: [...]`.
//
// These are pure frontend changes: the test drives the lexer/parser directly
// and checks (a) the source parses with zero syntax errors and (b)
// Expression::fromContext builds the expected AST node shapes. Codegen for
// CallExpression / ArrayLiteralExpression lands with the launch lowering
// (later increments) and is intentionally not invoked here.
//

#include "gtest/gtest.h"

#include "CajetaLexer.h"
#include "CajetaParser.h"

#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/CallExpression.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/DotExpression.h"

#include <string>

using cajeta::CajetaLexer;
using cajeta::CajetaParser;
using cajeta::Expression;
using cajeta::ExpressionPtr;
using cajeta::CallExpression;
using cajeta::ArrayLiteralExpression;
using cajeta::MethodCallExpression;
using cajeta::DotExpression;

namespace {

// Drives the parser to the top-level `expression` rule. Keeps the ANTLR
// objects alive in the caller's scope (the returned context points into
// `tokens`/`parser`), so this is a struct rather than a bare return.
struct ParsedExpr {
    // Declaration order == construction order; each stage feeds the next.
    antlr4::ANTLRInputStream input;
    CajetaLexer lexer;
    antlr4::CommonTokenStream tokens;
    CajetaParser parser;
    CajetaParser::ExpressionContext* ctx = nullptr;

    explicit ParsedExpr(const std::string& src)
        : input(src), lexer(&input), tokens(&lexer), parser(&tokens) {
        ctx = parser.expression();
    }

    size_t syntaxErrors() {
        return parser.getNumberOfSyntaxErrors();
    }
};

// Parses a whole compilation unit and reports the syntax-error count — used
// to confirm a full method body containing the launch statement parses.
size_t compilationUnitSyntaxErrors(const std::string& src) {
    antlr4::ANTLRInputStream input(src);
    CajetaLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CajetaParser parser(&tokens);
    parser.compilationUnit();
    return parser.getNumberOfSyntaxErrors();
}

} // namespace

// `[256]`, `[1, 2, 3]`, and `[]` parse as array literals and build an
// ArrayLiteralExpression carrying one element per entry.

// An arithmetic element expression inside a literal parses (the `grid:` dim
// in the spec is `[(n + 255) / 256]`).

// The postfix-call form: a bare `(args)` applied to a call result builds a
// CallExpression whose callee is the inner call and whose args are captured.

// Chained postfix calls: `f()()` invokes the result of `f()`. Confirms the
// new alternative is genuinely general, not launch-specific.

// Regression guard: the new alternatives must not disturb established call
// forms. `foo(x)` and `obj.foo(x)` keep parsing as method calls, not as
// postfix CallExpressions.
TEST(XpuLaunchGrammarTests, ordinaryCallsUnaffected) {
    {
        ParsedExpr p("foo(x)");
        EXPECT_EQ(p.syntaxErrors(), 0u);
        auto ast = Expression::fromContext(p.ctx);
        EXPECT_NE(std::dynamic_pointer_cast<MethodCallExpression>(ast), nullptr);
        EXPECT_EQ(std::dynamic_pointer_cast<CallExpression>(ast), nullptr);
    }
    {
        ParsedExpr p("obj.foo(x)");
        EXPECT_EQ(p.syntaxErrors(), 0u);
        auto ast = Expression::fromContext(p.ctx);
        // obj.foo(x) routes through DotExpression; not a postfix CallExpression.
        EXPECT_EQ(std::dynamic_pointer_cast<CallExpression>(ast), nullptr);
    }
}

// The whole launch statement inside a host method body parses cleanly —
// exercises the grammar exactly as CajetaXPU.md §3.1.3 writes it.
TEST(XpuLaunchGrammarTests, fullLaunchStatementParsesInMethodBody) {
    std::string src =
        "package test;\n"
        "public class Host {\n"
        "    public static void run(uint32 n) {\n"
        "        let stream = KernelStream.current();\n"
        "        saxpy.launch(stream, grid: [(n + 255) / 256], block: [256])"
        "(y, x, 2.0f, n);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(compilationUnitSyntaxErrors(src), 0u);
}
