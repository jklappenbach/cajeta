// parse-error-locality U1 (spec §2.2) — a syntax error damages its statement,
// not the file. Drives the generated parser directly; 1.1.5 drives `--lint`.

#include "gtest/gtest.h"

#include "CajetaLexer.h"
#include "CajetaParser.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::CajetaLexer;
using cajeta::CajetaParser;

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_PEL_DEVNULL "NUL"
#else
#  define CAJETA_PEL_DEVNULL "/dev/null"
#endif

struct SyntaxError {
    size_t line;
    size_t col;
    std::string message;
};

struct Survivors {
    int classes = 0;
    int methods = 0;
    int fields = 0;
    int locals = 0;
    int identifiers = 0;
    int errorNodes = 0;
};

class Collect : public antlr4::BaseErrorListener {
public:
    std::vector<SyntaxError> errors;
    void syntaxError(antlr4::Recognizer*, antlr4::Token*, size_t line,
                     size_t col, const std::string& msg,
                     std::exception_ptr) override {
        errors.push_back({line, col, msg});
    }
};

/** Parses one unit and reports what the tree still holds. */
struct Parsed {
    antlr4::ANTLRInputStream input;
    CajetaLexer lexer;
    antlr4::CommonTokenStream tokens;
    CajetaParser parser;
    Collect collect;
    CajetaParser::CompilationUnitContext* ctx = nullptr;

    explicit Parsed(const std::string& src)
        : input(src), lexer(&input), tokens(&lexer), parser(&tokens) {
        lexer.removeErrorListeners();
        lexer.addErrorListener(&collect);
        parser.removeErrorListeners();
        parser.addErrorListener(&collect);
        ctx = parser.compilationUnit();
    }

    size_t errors() const { return collect.errors.size(); }
    bool consumedEverything() { return tokens.index() == tokens.size() - 1; }

    Survivors survivors() {
        Survivors s;
        walk(ctx, s);
        return s;
    }

private:
    static void walk(antlr4::tree::ParseTree* t, Survivors& s) {
        if (dynamic_cast<CajetaParser::ClassDeclarationContext*>(t)) ++s.classes;
        else if (dynamic_cast<CajetaParser::MethodDeclarationContext*>(t)) ++s.methods;
        else if (dynamic_cast<CajetaParser::FieldDeclarationContext*>(t)) ++s.fields;
        else if (dynamic_cast<CajetaParser::LocalVariableDeclarationContext*>(t)) ++s.locals;
        else if (dynamic_cast<CajetaParser::IdentifierContext*>(t)) ++s.identifiers;
        else if (dynamic_cast<antlr4::tree::ErrorNode*>(t)) ++s.errorNodes;
        for (auto* c : t->children) walk(c, s);
    }
};

/** The spec §1.2 unit: a field and two methods, `second()` as given. */
std::string unit(const std::string& name, const std::string& second) {
    return "package demo;\n"
           "\n"
           "public final class " + name + " {\n"
           "    int32 total;\n"
           "\n"
           "    public int32 first() {\n"
           "        int32 a = 1;\n"
           "        return a;\n"
           "    }\n"
           "\n"
           "    public int32 second() {\n"
           "        " + second + "\n"
           "        return b;\n"
           "    }\n"
           "}\n";
}

const std::string kValid  = unit("Edit",   "int32 b = 2;");
const std::string kBroken = unit("Broken", "int32 b = 2");
const std::string kParen  = unit("Paren",  "int32 b = (2;");
const std::string kQuote  = unit("Quote",  "String s = \"abc;");
const std::string kBrace =
    "package demo;\n"
    "\n"
    "public final class Brace {\n"
    "    int32 total;\n"
    "\n"
    "    public int32 first() {\n"
    "        int32 a = 1;\n"
    "        return a;\n"
    "\n"
    "    public int32 second() {\n"
    "        int32 b = 2;\n"
    "        return b;\n"
    "    }\n"
    "}\n";
const std::string kTwo =
    "package demo;\n"
    "\n"
    "public final class Alpha {\n"
    "    public int32 run() {\n"
    "        int32 x = 1\n"
    "        return x;\n"
    "    }\n"
    "}\n"
    "\n"
    "public final class Beta {\n"
    "    public int32 go() {\n"
    "        int32 y = 2;\n"
    "        return y;\n"
    "    }\n"
    "}\n";

void expectWholeClass(const Survivors& s) {
    EXPECT_EQ(1, s.classes);
    EXPECT_EQ(2, s.methods);
    EXPECT_EQ(1, s.fields);
    EXPECT_EQ(2, s.locals);
}

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    return (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        CAJETA_SOURCE_ROOT_DEFAULT;
#else
        ".";
#endif
}

std::string compilerBinary() { return sourceRoot() + "/build/src/cajeta"; }
bool haveCompiler() { return fs::exists(compilerBinary()); }

fs::path freshTempDir() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_pel_" + std::to_string(rng()));
    fs::create_directories(base / "demo");
    return base;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/** Lines of the JSON diagnostic stream for one `--lint` that carry `code:"syntax"`. */
std::vector<std::string> syntaxDiagnosticsOf(const std::string& name, const std::string& text) {
    auto root = freshTempDir();
    auto file = root / "demo" / (name + ".cajeta");
    std::ofstream(file) << text;
    auto err = root / "lint.err";
    (void) std::system((compilerBinary() + " --lint " + file.string()
                        + " --source-root " + root.string()
                        + " --diag-format=json > " CAJETA_PEL_DEVNULL " 2> "
                        + err.string()).c_str());
    std::vector<std::string> out;
    std::string all = slurp(err);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(pos, nl - pos);
        if (line.find("\"code\":\"syntax\"") != std::string::npos) out.push_back(line);
        pos = nl + 1;
    }
    fs::remove_all(root);
    return out;
}

}  // namespace

// Control (spec §1.2 valid row): everything reduces, nothing is reported.
TEST(ParseErrorLocalityTests, AValidUnitParsesWhole) {
    Parsed p(kValid);
    EXPECT_EQ(0u, p.errors());
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    expectWholeClass(s);
    EXPECT_EQ(9, s.identifiers);
    EXPECT_EQ(0, s.errorNodes);
}

// 1.1.1 / 1.1.2 / spec 2.2.1
TEST(ParseErrorLocalityTests, AMissingSemicolonKeepsEveryOtherDeclaration) {
    Parsed p(kBroken);
    ASSERT_EQ(1u, p.errors());
    EXPECT_EQ(13u, p.collect.errors[0].line);
    EXPECT_EQ("missing ';' at 'return'", p.collect.errors[0].message);
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    expectWholeClass(s);
    EXPECT_EQ(9, s.identifiers);
    EXPECT_EQ(1, s.errorNodes);
}

// spec 2.2.2
TEST(ParseErrorLocalityTests, AnUnclosedParenKeepsEveryOtherDeclaration) {
    Parsed p(kParen);
    EXPECT_EQ(1u, p.errors());
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    expectWholeClass(s);
    EXPECT_EQ(9, s.identifiers);
}

// spec 2.2.2
TEST(ParseErrorLocalityTests, AnUnterminatedStringKeepsEveryOtherDeclaration) {
    Parsed p(kQuote);
    EXPECT_GE(p.errors(), 1u);
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    expectWholeClass(s);
    EXPECT_GE(s.identifiers, 9);
}

// spec 2.2.3 — the next method is absorbed; the class and its names survive.
TEST(ParseErrorLocalityTests, AMissingBraceKeepsTheClassAndItsIdentifiers) {
    Parsed p(kBrace);
    EXPECT_GE(p.errors(), 1u);
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    EXPECT_EQ(1, s.classes);
    EXPECT_GE(s.methods, 1);
    EXPECT_EQ(1, s.fields);
    EXPECT_EQ(2, s.locals);
    EXPECT_EQ(9, s.identifiers);
}

// spec 2.2.4
TEST(ParseErrorLocalityTests, ABrokenFirstClassLeavesTheSecondWhole) {
    Parsed p(kTwo);
    EXPECT_EQ(1u, p.errors());
    EXPECT_TRUE(p.consumedEverything());
    auto s = p.survivors();
    EXPECT_EQ(2, s.classes);
    EXPECT_EQ(2, s.methods);
    EXPECT_EQ(9, s.identifiers);
}

// 1.1.2 / spec 2.2.7 — no message quotes the file from its first token.
TEST(ParseErrorLocalityTests, NoSyntaxErrorQuotesTheFileFromItsFirstToken) {
    for (const auto* src : {&kBroken, &kParen, &kQuote, &kBrace, &kTwo}) {
        Parsed p(*src);
        ASSERT_GE(p.errors(), 1u);
        for (const auto& e : p.collect.errors) {
            EXPECT_EQ(std::string::npos,
                      e.message.find("no viable alternative at input 'package"))
                << e.message.substr(0, 80);
            EXPECT_GT(e.line, 1u) << e.message.substr(0, 80);
        }
    }
}

// 1.1.3 / spec 2.2.6 — no decision at file scope needs the whole file.
TEST(ParseErrorLocalityTests, TheTopLevelDecisionIsLocal) {
    class Ambiguities : public antlr4::DiagnosticErrorListener {
    public:
        std::vector<std::string> decisions;
        Ambiguities() : antlr4::DiagnosticErrorListener(true) {}
        void reportAmbiguity(antlr4::Parser* recognizer, const antlr4::dfa::DFA& dfa,
                             size_t, size_t, bool, const antlrcpp::BitSet&,
                             antlr4::atn::ATNConfigSet*) override {
            decisions.push_back(recognizer->getRuleNames()[dfa.atnStartState->ruleIndex]);
        }
    };
    antlr4::ANTLRInputStream input(kValid);
    CajetaLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CajetaParser parser(&tokens);
    parser.removeErrorListeners();
    Ambiguities ambiguities;
    parser.addErrorListener(&ambiguities);
    parser.getInterpreter<antlr4::atn::ParserATNSimulator>()
        ->setPredictionMode(antlr4::atn::PredictionMode::LL_EXACT_AMBIG_DETECTION);
    parser.compilationUnit();
    for (const auto& rule : ambiguities.decisions) {
        EXPECT_NE("compilationUnit", rule);
        EXPECT_NE("scriptMember", rule);
    }
}

// 1.1.5 / spec 2.2.7 — the JSON diagnostic points at the failing token.
TEST(ParseErrorLocalityTests, ALintSyntaxDiagnosticPointsAtTheFailingToken) {
    if (!haveCompiler()) GTEST_SKIP() << "no build/src/cajeta";
    auto lines = syntaxDiagnosticsOf("Broken", kBroken);
    ASSERT_EQ(1u, lines.size());
    EXPECT_NE(std::string::npos, lines[0].find("\"line\":13,")) << lines[0];
    EXPECT_EQ(std::string::npos,
              lines[0].find("no viable alternative at input 'package")) << lines[0].substr(0, 160);
    EXPECT_TRUE(syntaxDiagnosticsOf("Edit", kValid).empty());
}
