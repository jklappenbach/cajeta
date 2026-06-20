//
// YAML-header parser (frontmatter subset). See Yaml.h and
// docs/specs/yaml-frontmatter-spec.md §3.
//
#include "cajeta/buildtool/Yaml.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        // A significant logical line: blank and full-comment lines are dropped
        // during scanning, so each entry carries real content.
        struct Line {
            int indent;          // number of leading spaces (tabs are an error)
            llvm::StringRef text; // content after the indent, trailing space trimmed
            int lineNo;          // 1-based source line
        };

        llvm::Error makeError(int line, const llvm::Twine& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                ("line " + llvm::Twine(line) + ": " + msg).str());
        }

        // Split the header into significant lines. Leading-whitespace tabs are
        // rejected (YAML forbids tab indentation); blank and comment-only lines
        // are skipped. These byte scans are isolated so a SIMD newline scan can
        // replace them later (spec §5).
        llvm::Expected<std::vector<Line>> scanLines(std::string_view header) {
            std::vector<Line> out;
            size_t pos = 0;
            int lineNo = 1;
            while (pos < header.size()) {
                size_t nl = header.find('\n', pos);
                size_t end = (nl == std::string_view::npos) ? header.size() : nl;
                llvm::StringRef phys(header.data() + pos, end - pos);
                if (phys.ends_with("\r")) {
                    phys = phys.drop_back(1);
                }

                // Measure leading whitespace; a tab in the indent is an error.
                size_t firstNonWs = 0;
                bool sawTab = false;
                while (firstNonWs < phys.size() &&
                       (phys[firstNonWs] == ' ' || phys[firstNonWs] == '\t')) {
                    if (phys[firstNonWs] == '\t') {
                        sawTab = true;
                    }
                    ++firstNonWs;
                }
                llvm::StringRef rest = phys.drop_front(firstNonWs).rtrim();
                // Blank or comment-only lines are insignificant (tabs in an
                // otherwise blank line are harmless).
                if (!rest.empty() && rest.front() != '#') {
                    if (sawTab) {
                        return makeError(lineNo, "tab indentation is not allowed");
                    }
                    out.push_back({static_cast<int>(firstNonWs), rest, lineNo});
                }

                if (nl == std::string_view::npos) {
                    break;
                }
                pos = nl + 1;
                ++lineNo;
            }
            return out;
        }

        // Parse a single-quoted scalar starting at v[0]=='\''. `''` is a literal
        // quote. Returns the unquoted string; anything after the closing quote
        // (e.g. an inline comment) is ignored.
        llvm::Expected<llvm::json::Value> parseSingleQuoted(llvm::StringRef v, int lineNo) {
            std::string s;
            size_t i = 1;
            while (i < v.size()) {
                if (v[i] == '\'') {
                    if (i + 1 < v.size() && v[i + 1] == '\'') {
                        s.push_back('\'');
                        i += 2;
                        continue;
                    }
                    return llvm::json::Value(std::move(s));
                }
                s.push_back(v[i]);
                ++i;
            }
            return makeError(lineNo, "unterminated single-quoted string");
        }

        // Parse a double-quoted scalar starting at v[0]=='"', handling the common
        // backslash escapes. Text after the closing quote is ignored.
        llvm::Expected<llvm::json::Value> parseDoubleQuoted(llvm::StringRef v, int lineNo) {
            std::string s;
            size_t i = 1;
            while (i < v.size()) {
                char c = v[i];
                if (c == '"') {
                    return llvm::json::Value(std::move(s));
                }
                if (c == '\\' && i + 1 < v.size()) {
                    char e = v[i + 1];
                    switch (e) {
                        case 'n': s.push_back('\n'); break;
                        case 't': s.push_back('\t'); break;
                        case 'r': s.push_back('\r'); break;
                        case '"': s.push_back('"'); break;
                        case '\\': s.push_back('\\'); break;
                        case '0': s.push_back('\0'); break;
                        default: s.push_back(e); break; // unknown escape → literal
                    }
                    i += 2;
                    continue;
                }
                s.push_back(c);
                ++i;
            }
            return makeError(lineNo, "unterminated double-quoted string");
        }

        // Type a plain (unquoted) scalar per spec §3.1, after stripping any
        // trailing inline comment (a '#' preceded by whitespace).
        llvm::json::Value typePlainScalar(llvm::StringRef v) {
            // Strip an inline comment: first '#' that is at the start or follows
            // whitespace. A '#' glued to a non-space char (e.g. a URL fragment)
            // is literal.
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '#' && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')) {
                    v = v.take_front(i);
                    break;
                }
            }
            v = v.rtrim();

            if (v.empty() || v == "null" || v == "~") {
                return llvm::json::Value(nullptr);
            }
            if (v == "true") {
                return llvm::json::Value(true);
            }
            if (v == "false") {
                return llvm::json::Value(false);
            }
            int64_t iv;
            if (!v.getAsInteger(10, iv)) {
                return llvm::json::Value(iv);
            }
            double dv;
            if (!v.getAsDouble(dv)) {
                return llvm::json::Value(dv);
            }
            return llvm::json::Value(v.str());
        }

        // Parse a scalar value region (already trimmed on the left).
        llvm::Expected<llvm::json::Value> parseScalar(llvm::StringRef v, int lineNo) {
            if (v.empty()) {
                return llvm::json::Value(nullptr);
            }
            if (v.front() == '\'') {
                return parseSingleQuoted(v, lineNo);
            }
            if (v.front() == '"') {
                return parseDoubleQuoted(v, lineNo);
            }
            return typePlainScalar(v);
        }

        // Parse a block of mapping entries all at `indent`. Advances `idx` past
        // every line it consumes (including nested children).
        llvm::Expected<llvm::json::Value>
        parseMapping(const std::vector<Line>& lines, size_t& idx, int indent) {
            llvm::json::Object obj;
            while (idx < lines.size() && lines[idx].indent == indent) {
                const Line& cur = lines[idx];
                size_t colon = cur.text.find(':');
                if (colon == llvm::StringRef::npos) {
                    return makeError(cur.lineNo, "expected 'key: value' mapping entry");
                }
                llvm::StringRef key = cur.text.take_front(colon).rtrim();
                if (key.empty()) {
                    return makeError(cur.lineNo, "empty mapping key");
                }
                llvm::StringRef valueRegion = cur.text.drop_front(colon + 1).ltrim();
                ++idx;

                if (valueRegion.empty()) {
                    // Either a nested block (deeper indent) or a null value.
                    if (idx < lines.size() && lines[idx].indent > indent) {
                        if (lines[idx].text.starts_with("-")) {
                            // Block sequences arrive in unit D.Y3.
                            return makeError(lines[idx].lineNo,
                                             "block sequences are not yet supported");
                        }
                        auto child = parseMapping(lines, idx, lines[idx].indent);
                        if (!child) {
                            return child.takeError();
                        }
                        obj[key] = std::move(*child);
                    } else {
                        obj[key] = llvm::json::Value(nullptr);
                    }
                } else {
                    auto val = parseScalar(valueRegion, cur.lineNo);
                    if (!val) {
                        return val.takeError();
                    }
                    obj[key] = std::move(*val);
                }
            }
            return llvm::json::Value(std::move(obj));
        }

    } // namespace

    llvm::Expected<llvm::json::Value> parseYaml(std::string_view header) {
        auto lines = scanLines(header);
        if (!lines) {
            return lines.takeError();
        }
        if (lines->empty()) {
            return llvm::json::Value(llvm::json::Object{});
        }

        size_t idx = 0;
        int baseIndent = lines->front().indent;
        auto value = parseMapping(*lines, idx, baseIndent);
        if (!value) {
            return value.takeError();
        }
        if (idx < lines->size()) {
            return makeError((*lines)[idx].lineNo, "unexpected indentation");
        }
        return value;
    }

} // namespace cajeta::buildtool
