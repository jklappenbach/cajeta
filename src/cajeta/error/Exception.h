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
    public:
        Exception() { }

        Exception(string message, string errorId) {
            this->message = message;
            this->errorId = errorId;
        }

        string getMessage() { return message; }

        string getErrorId() { return errorId; }
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