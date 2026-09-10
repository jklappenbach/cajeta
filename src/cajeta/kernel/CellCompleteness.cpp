#include "cajeta/kernel/CellCompleteness.h"

#include <antlr4-runtime.h>

#include <cctype>

#include "CajetaLexer.h"
#include "CajetaParser.h"

namespace cajeta::kernel {

    namespace {

        // Records only what the verdict turns on: whether there was an error at all,
        // and whether the FIRST one was the parser running out of input.
        class FirstErrorListener : public antlr4::BaseErrorListener {
        public:
            bool sawError = false;
            bool firstAtEof = false;

            void syntaxError(antlr4::Recognizer* /*recognizer*/,
                             antlr4::Token* offendingSymbol,
                             size_t /*line*/, size_t /*charPositionInLine*/,
                             const std::string& /*msg*/,
                             std::exception_ptr /*e*/) override {
                if (sawError) return;
                sawError = true;
                firstAtEof = offendingSymbol != nullptr
                          && offendingSymbol->getType() == antlr4::Token::EOF;
            }
        };

        struct Scan {
            bool openTextBlock = false;  // ran off the end inside """ ... """
            bool openComment = false;    // ran off the end inside /* ... */
            int braceDepth = 0;
        };

        // Only a TEXT_BLOCK and a block comment may legally span a line break, and the
        // LEXER reports both at their opening token, so an "is the parser at EOF" test
        // cannot see them. An unterminated ordinary string is invalid, not incomplete.
        Scan scanSource(const std::string& s) {
            Scan out;
            enum { Code, LineComment, BlockComment, Str, Chr, TextBlock } state = Code;
            for (size_t i = 0; i < s.size(); ++i) {
                char c = s[i];
                char next = (i + 1 < s.size()) ? s[i + 1] : '\0';
                bool tripleHere = c == '"' && next == '"'
                               && (i + 2 < s.size()) && s[i + 2] == '"';
                switch (state) {
                    case Code:
                        if (c == '/' && next == '/') { state = LineComment; ++i; }
                        else if (c == '/' && next == '*') { state = BlockComment; ++i; }
                        else if (tripleHere) { state = TextBlock; i += 2; }
                        else if (c == '"') state = Str;
                        else if (c == '\'') state = Chr;
                        else if (c == '{') ++out.braceDepth;
                        else if (c == '}') --out.braceDepth;
                        break;
                    case LineComment:
                        if (c == '\n') state = Code;
                        break;
                    case BlockComment:
                        if (c == '*' && next == '/') { state = Code; ++i; }
                        break;
                    case TextBlock:
                        if (c == '\\') ++i;
                        else if (tripleHere) { state = Code; i += 2; }
                        break;
                    case Str:
                        if (c == '\\') ++i;
                        else if (c == '"') state = Code;
                        else if (c == '\n') state = Code;  // cannot span lines
                        break;
                    case Chr:
                        if (c == '\\') ++i;
                        else if (c == '\'') state = Code;
                        else if (c == '\n') state = Code;
                        break;
                }
            }
            out.openTextBlock = (state == TextBlock);
            out.openComment = (state == BlockComment);
            if (out.braceDepth < 0) out.braceDepth = 0;
            return out;
        }

    }  // namespace

    const char* completenessName(Completeness c) {
        switch (c) {
            case Completeness::Complete:   return "complete";
            case Completeness::Incomplete: return "incomplete";
            case Completeness::Invalid:    return "invalid";
        }
        return "unknown";
    }

    Completeness classifyCell(const std::string& source, std::string* indent) {
        if (indent) indent->clear();

        bool blank = true;
        for (char c : source) {
            if (!std::isspace(static_cast<unsigned char>(c))) { blank = false; break; }
        }
        if (blank) return Completeness::Complete;

        Scan scan = scanSource(source);
        auto continuation = [&]() {
            if (indent) indent->assign(static_cast<size_t>(scan.braceDepth) * 4, ' ');
            return Completeness::Incomplete;
        };
        if (scan.openTextBlock || scan.openComment) return continuation();

        antlr4::ANTLRInputStream input(source);
        CajetaLexer lexer(&input);
        FirstErrorListener listener;
        lexer.removeErrorListeners();
        lexer.addErrorListener(&listener);

        antlr4::CommonTokenStream tokens(&lexer);
        tokens.fill();

        CajetaParser parser(&tokens);
        parser.removeErrorListeners();
        parser.addErrorListener(&listener);
        parser.compilationUnit();

        if (!listener.sawError) return Completeness::Complete;
        if (listener.firstAtEof || scan.braceDepth > 0) return continuation();
        return Completeness::Invalid;
    }

}  // namespace cajeta::kernel
