//
// Session 1 / Step 1.6 — parse-level tests for new syntax landing in this
// rollout phase:
//   - `#` operator in value-prefix position
//   - `#T` on parameter types
//   - `#T` on return types
//   - `struct` declarations
//   - `@BigEndian` / `@LittleEndian` / `@Align(natural)` annotations on structs
//
// These tests run the lexer + parser directly and confirm the parser accepts
// (or rejects) the input. They DO NOT run codegen — the AST and semantic
// machinery for these constructs lands in later sessions per
// `ImplementationStatus.md`.
//

#include "gtest/gtest.h"

#include <antlr4-runtime.h>
#include "CajetaLexer.h"
#include "CajetaParser.h"

#include <sstream>
#include <string>
#include <memory>

namespace {

// Collect parser/lexer errors instead of printing to stderr; we want the test
// to fail when the *parser* errors out, not the whole process to spew noise.
struct CollectingErrorListener : antlr4::BaseErrorListener {
    int errorCount = 0;
    void syntaxError(antlr4::Recognizer* /*recognizer*/, antlr4::Token* /*offendingSymbol*/,
                      size_t /*line*/, size_t /*charPositionInLine*/,
                      const std::string& /*msg*/, std::exception_ptr /*e*/) override {
        errorCount++;
    }
};

// Parse `source` (a complete compilation unit) and return the number of
// syntax errors the parser reported. Zero means a clean parse.
int parseErrorCount(const std::string& source) {
    antlr4::ANTLRInputStream input(source);
    cajeta::CajetaLexer lexer(&input);
    CollectingErrorListener lexErr;
    lexer.removeErrorListeners();
    lexer.addErrorListener(&lexErr);

    antlr4::CommonTokenStream tokens(&lexer);
    cajeta::CajetaParser parser(&tokens);
    CollectingErrorListener parseErr;
    parser.removeErrorListeners();
    parser.addErrorListener(&parseErr);

    // Drive the parser to its start rule; we just need to know whether it can
    // consume the whole input without errors.
    parser.compilationUnit();
    return lexErr.errorCount + parseErr.errorCount;
}

std::string compilationUnit(const std::string& classBody) {
    return "package test;\n"
           "public class T {\n"
           + classBody +
           "}\n";
}

} // namespace

// --- `#` as a value-prefix operator -----------------------------------------

TEST(Session1Parse, hashOnAssignmentRhs) {
    auto src = compilationUnit(
        "public static int32 run() {\n"
        "    String a = \"hello\";\n"
        "    String b = #a;\n"
        "    return 0;\n"
        "}\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, hashOnMethodCallArgument) {
    auto src = compilationUnit(
        "public static int32 run() {\n"
        "    String a = \"hello\";\n"
        "    consume(#a);\n"
        "    return 0;\n"
        "}\n"
        "public static void consume(String s) { }\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, hashOnReturnExpression) {
    auto src = compilationUnit(
        "public static String makeString() {\n"
        "    String s = \"hi\";\n"
        "    return #s;\n"
        "}\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

// --- `#T` on parameter types ------------------------------------------------

TEST(Session1Parse, hashOnParameterType) {
    auto src = compilationUnit(
        "public static void take(#String s) { }\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, hashOnMultipleParameterTypes) {
    auto src = compilationUnit(
        "public static void take(#String a, int32 b, #String c) { }\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

// --- `#T` on return types ---------------------------------------------------

TEST(Session1Parse, hashOnReturnType) {
    auto src = compilationUnit(
        "public static #String make() {\n"
        "    return \"hi\";\n"
        "}\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

// --- `struct` declarations --------------------------------------------------

TEST(Session1Parse, simpleStructDeclaration) {
    auto src =
        "package test;\n"
        "public struct Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, structAlongsideClass) {
    auto src =
        "package test;\n"
        "public struct Header {\n"
        "    int32 version;\n"
        "}\n"
        "public class Handler {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, structWithStringField) {
    // Inline length-prefixed String — parser doesn't enforce layout rules yet
    // but should accept the declaration as a regular field.
    auto src =
        "package test;\n"
        "public struct UserRecord {\n"
        "    int64 id;\n"
        "    String username;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

// --- Endianness / alignment annotations on structs --------------------------

TEST(Session1Parse, bigEndianAnnotationOnStruct) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public struct RpcHeader {\n"
        "    int32 magic;\n"
        "    int16 version;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, littleEndianAnnotationOnStruct) {
    auto src =
        "package test;\n"
        "@LittleEndian\n"
        "public struct Record {\n"
        "    int32 a;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, alignAnnotationOnStruct) {
    auto src =
        "package test;\n"
        "@Align(natural)\n"
        "public struct LocalRecord {\n"
        "    int32 a;\n"
        "    int64 b;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, multipleAnnotationsOnStruct) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "@Align(natural)\n"
        "public struct WireRecord {\n"
        "    int32 a;\n"
        "}\n";
    EXPECT_EQ(parseErrorCount(src), 0);
}

// --- Invalid syntax: parser should reject -----------------------------------

TEST(Session1Parse, invalidHashAsBinaryOperator) {
    // `#` is a prefix operator only; using it as an infix operator should fail.
    auto src = compilationUnit(
        "public static int32 run() {\n"
        "    int32 a = 1 # 2;\n"
        "    return a;\n"
        "}\n");
    EXPECT_GT(parseErrorCount(src), 0);
}

TEST(Session1Parse, invalidStructWithoutBody) {
    // A struct declaration without a body is a syntax error.
    auto src =
        "package test;\n"
        "public struct Header;\n";
    EXPECT_GT(parseErrorCount(src), 0);
}

TEST(Session1Parse, invalidDoubleHashOnExpression) {
    // `##x` would attempt to apply the move operator twice — semantically
    // meaningless but the parser tolerates `# (#x)` as a prefix of a prefix.
    // This test pins current behavior: the parser accepts it; later sessions
    // will surface a static-analysis error.
    auto src = compilationUnit(
        "public static int32 run() {\n"
        "    String a = \"hi\";\n"
        "    String b = ##a;\n"
        "    return 0;\n"
        "}\n");
    EXPECT_EQ(parseErrorCount(src), 0);
}

TEST(Session1Parse, invalidHashOnLocalVariableType) {
    // `#T x` on a local declaration isn't part of the spec — locals don't have
    // a "this parameter takes ownership" form. Should fail to parse.
    auto src = compilationUnit(
        "public static int32 run() {\n"
        "    #String s = \"hi\";\n"
        "    return 0;\n"
        "}\n");
    EXPECT_GT(parseErrorCount(src), 0);
}
