#include "cajeta/buildtool/JsonC.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace cajeta::buildtool {

    namespace {

        // Inside a JSON string literal, a backslash escapes the next
        // character. We only need to track this to know when a `"` ends
        // the literal vs. is escaped; we don't need to interpret the
        // escape itself.
        bool isStringStart(char c) { return c == '"'; }

        // Replace one character of input with whitespace in output. Keep
        // newlines as newlines so line numbers in error reports survive
        // preprocessing.
        char blank(char c) { return c == '\n' ? '\n' : ' '; }

    } // namespace

    std::string preprocessJsonC(std::string_view source) {
        std::string out(source.size(), ' ');

        size_t i = 0;
        const size_t n = source.size();

        // Pass 1: blank out comments and string-aware copy of the rest.
        // We can't strip trailing commas in the same pass safely (the
        // lookahead-then-rewrite would interact poorly with comment
        // blanking inside the lookahead window). Two passes is clearer.
        while (i < n) {
            char c = source[i];

            if (isStringStart(c)) {
                // Copy the entire string literal verbatim, respecting
                // backslash escapes. JSON strings cannot contain raw
                // newlines, but a `\"` inside the string must not end
                // the literal.
                out[i] = c;
                ++i;
                while (i < n) {
                    char d = source[i];
                    out[i] = d;
                    if (d == '\\' && i + 1 < n) {
                        // Copy escape char and the escaped char verbatim.
                        out[i + 1] = source[i + 1];
                        i += 2;
                        continue;
                    }
                    if (d == '"') {
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }

            // `//` line comment — blank to end of line (preserve the
            // terminating newline so line numbers stay aligned).
            if (c == '/' && i + 1 < n && source[i + 1] == '/') {
                while (i < n && source[i] != '\n') {
                    out[i] = ' ';
                    ++i;
                }
                continue;
            }

            // `/* ... */` block comment — blank everything between
            // (newlines preserved).
            if (c == '/' && i + 1 < n && source[i + 1] == '*') {
                out[i] = ' ';
                out[i + 1] = ' ';
                i += 2;
                while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/')) {
                    out[i] = blank(source[i]);
                    ++i;
                }
                if (i + 1 < n) {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    i += 2;
                } else if (i < n) {
                    // Unterminated block comment — blank the last char
                    // so the cleaned source ends cleanly. The downstream
                    // parser will likely produce a structural error,
                    // which is fine (better than a silent acceptance).
                    out[i] = blank(source[i]);
                    ++i;
                }
                continue;
            }

            out[i] = c;
            ++i;
        }

        // Pass 2: strip trailing commas. A comma is "trailing" when the
        // next non-whitespace character (in the cleaned source) is `}`
        // or `]`. We scan forward from each comma; if we hit a closer
        // before any other token, replace the comma with a space.
        for (size_t j = 0; j < n; ++j) {
            if (out[j] != ',') continue;
            // Skip whitespace.
            size_t k = j + 1;
            while (k < n) {
                char c = out[k];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++k;
                    continue;
                }
                break;
            }
            if (k < n && (out[k] == '}' || out[k] == ']')) {
                out[j] = ' ';
            }
        }

        return out;
    }

    llvm::Expected<llvm::json::Value> parseJsonC(std::string_view source) {
        std::string cleaned = preprocessJsonC(source);
        return llvm::json::parse(cleaned);
    }

    llvm::Expected<llvm::json::Value> parseJsonCFile(const std::string& path) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) {
            return llvm::createStringError(
                buf.getError(),
                "cannot open manifest file '" + path + "': " +
                buf.getError().message());
        }
        auto val = parseJsonC((*buf)->getBuffer());
        if (!val) {
            // Wrap the underlying parse error with the file path so
            // tooling shows where it came from.
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << "in '" << path << "': " << val.takeError();
            return llvm::createStringError(llvm::inconvertibleErrorCode(), msg);
        }
        return std::move(*val);
    }

} // namespace cajeta::buildtool
