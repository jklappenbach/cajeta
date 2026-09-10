#include "cajeta/buildtool/JsonC.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace cajeta::buildtool {

    namespace {

        bool isStringStart(char c) { return c == '"'; }

        // Blank one character, keeping a newline a newline so line numbers survive.
        char blank(char c) { return c == '\n' ? '\n' : ' '; }

    } // namespace

    std::string preprocessJsonC(std::string_view source) {
        std::string out(source.size(), ' ');

        size_t i = 0;
        const size_t n = source.size();

        // Pass 1 blanks comments and copies the rest. Trailing commas need a second
        // pass: their lookahead window would otherwise span un-blanked comments.
        while (i < n) {
            char c = source[i];

            if (isStringStart(c)) {
                // The whole literal is copied verbatim; a `\"` must not end it.
                out[i] = c;
                ++i;
                while (i < n) {
                    char d = source[i];
                    out[i] = d;
                    if (d == '\\' && i + 1 < n) {
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

            if (c == '/' && i + 1 < n && source[i + 1] == '/') {
                while (i < n && source[i] != '\n') {
                    out[i] = ' ';
                    ++i;
                }
                continue;
            }

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
                    // An unterminated block comment is left to fail downstream as a
                    // structural error, rather than being silently accepted here.
                    out[i] = blank(source[i]);
                    ++i;
                }
                continue;
            }

            out[i] = c;
            ++i;
        }

        // Pass 2: a comma is trailing when the next non-whitespace character in the
        // cleaned source is a closer, and is then replaced by a space.
        for (size_t j = 0; j < n; ++j) {
            if (out[j] != ',') continue;
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
            // The path is wrapped in so tooling can show where the error came from.
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << "in '" << path << "': " << val.takeError();
            return llvm::createStringError(llvm::inconvertibleErrorCode(), msg);
        }
        return std::move(*val);
    }

} // namespace cajeta::buildtool
