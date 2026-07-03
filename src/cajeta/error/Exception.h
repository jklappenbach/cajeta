//
// Created by James Klappenbach on 11/6/22.
//

#pragma once

#include <string>

using namespace std;

namespace cajeta {

    class Exception {
    protected:
        string message;
        string errorId;
        string file;
        int line = -1;    // 1-based; <= 0 means "no location"
        int column = -1;  // 1-based
    public:
        Exception() { }

        Exception(string message, string errorId) {
            this->message = message;
            this->errorId = errorId;
        }

        // Located form (located-semantic-diagnostics): 1-based line/column.
        Exception(string message, string errorId, string file, int line, int column) {
            this->message = message;
            this->errorId = errorId;
            this->file = file;
            this->line = line;
            this->column = column;
        }

        string getMessage() { return message; }

        string getErrorId() { return errorId; }

        const string& getFile() const { return file; }

        int getLine() const { return line; }

        int getColumn() const { return column; }

        bool hasLocation() const { return line > 0; }
    };

    // Thrown after parsing when the user source has syntax errors. The per-error
    // diagnostics were already reported during the parse (NDJSON in json mode,
    // ANTLR console text otherwise), so the top-level handler fails the compile
    // without re-emitting. Aborts before the semantic visitor walks ANTLR's
    // malformed error-recovery tree, which segfaulted on some inputs.
    class SyntaxErrorException : public Exception {
    public:
        explicit SyntaxErrorException(int count)
            : Exception("source has " + std::to_string(count)
                        + " syntax error(s)", "syntax") {}
    };

} // code