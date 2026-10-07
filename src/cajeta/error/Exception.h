// Created by James Klappenbach on 11/6/22.

#pragma once

#include <string>

using namespace std;

namespace cajeta {

    // Where generated code came from: `via` names the generator, `from` its source position.
    struct GeneratedOrigin {
        string via;
        string fromFile;
        int fromLine = -1;
        int fromColumn = -1;
    };

    class Exception {
    protected:
        string message;
        string errorId;
        string file;
        int line = -1;    // 1-based; <= 0 means "no location"
        int column = -1;  // 1-based
        bool scriptRemapped = false;  // see markScriptRemapped()
        GeneratedOrigin generated;

        // Moves the location to the active generated-code site, when there is one.
        void stampGenerated();
    public:
        Exception() { }

        Exception(string message, string errorId) {
            this->message = message;
            this->errorId = errorId;
            stampGenerated();
        }

        // Located form (located-semantic-diagnostics): 1-based line/column.
        Exception(string message, string errorId, string file, int line, int column) {
            this->message = message;
            this->errorId = errorId;
            this->file = file;
            this->line = line;
            this->column = column;
            stampGenerated();
        }

        string getMessage() const { return message; }

        string getErrorId() const { return errorId; }

        const string& getFile() const { return file; }

        int getLine() const { return line; }

        int getColumn() const { return column; }

        bool hasLocation() const { return line > 0; }

        const GeneratedOrigin& getGenerated() const { return generated; }
        void setGenerated(const GeneratedOrigin& g) { generated = g; }

        // The remap flag makes the rewrite once-only: nested codegen rethrows through several remap boundaries.
        void setLocation(const string& f, int l, int c) {
            file = f;
            line = l;
            column = c;
        }
        bool isScriptRemapped() const { return scriptRemapped; }
        void markScriptRemapped() { scriptRemapped = true; }
    };

    // Thrown after parsing when the source has syntax errors the parse already reported.
    // Aborts before the semantic visitor walks ANTLR's malformed error-recovery tree.
    class SyntaxErrorException : public Exception {
    public:
        explicit SyntaxErrorException(int count)
            : Exception("source has " + std::to_string(count)
                        + " syntax error(s)", "syntax") {}
    };

} // code