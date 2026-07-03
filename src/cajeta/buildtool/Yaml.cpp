//
// YAML-header parser (frontmatter subset). See Yaml.h and
// specs/archive/yaml-frontmatter-spec.md §3.
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

        // True for a block-sequence entry line: `- item` or a bare `-`.
        bool isSeqItem(llvm::StringRef text) {
            return text == "-" || text.starts_with("- ");
        }

        // Mutually-recursive parsers (mappings, sequences, scalars can nest).
        llvm::Expected<llvm::json::Value> parseScalar(llvm::StringRef v, int lineNo);
        llvm::Expected<llvm::json::Value>
        parseBlock(const std::vector<Line>& lines, size_t& idx, int indent);

        // Split the header into significant lines. Leading-whitespace tabs are
        // rejected (YAML forbids tab indentation); blank and comment-only lines
        // are skipped. These byte scans are isolated so a SIMD newline scan can
        // replace them later (spec §5).
        llvm::Expected<std::vector<Line>> scanLines(std::string_view header, int firstLine) {
            std::vector<Line> out;
            size_t pos = 0;
            int lineNo = firstLine;
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

        // Find the index of the top-level `]` matching the `[` at v[0], honoring
        // quotes and nested brackets. Returns npos if unterminated.
        size_t findFlowClose(llvm::StringRef v) {
            int depth = 0;
            bool inS = false, inD = false;
            for (size_t i = 0; i < v.size(); ++i) {
                char c = v[i];
                if (inS) {
                    if (c == '\'') inS = false;
                } else if (inD) {
                    if (c == '\\') { ++i; }
                    else if (c == '"') inD = false;
                } else if (c == '\'') {
                    inS = true;
                } else if (c == '"') {
                    inD = true;
                } else if (c == '[') {
                    ++depth;
                } else if (c == ']') {
                    if (--depth == 0) return i;
                }
            }
            return llvm::StringRef::npos;
        }

        // Parse a flow sequence `[a, b, c]` (v[0]=='['). Items are split on
        // top-level commas (respecting quotes/nested brackets) and parsed as
        // scalars, so typing, quoting, and nested flow sequences all carry.
        llvm::Expected<llvm::json::Value> parseFlowSequence(llvm::StringRef v, int lineNo) {
            size_t close = findFlowClose(v);
            if (close == llvm::StringRef::npos) {
                return makeError(lineNo, "unterminated flow sequence");
            }
            llvm::json::Array arr;
            llvm::StringRef inner = v.substr(1, close - 1);

            int depth = 0;
            bool inS = false, inD = false;
            size_t itemStart = 0;
            auto emit = [&](size_t end) -> llvm::Error {
                llvm::StringRef item = inner.substr(itemStart, end - itemStart).trim();
                if (!item.empty()) {
                    auto val = parseScalar(item, lineNo);
                    if (!val) return val.takeError();
                    arr.push_back(std::move(*val));
                }
                return llvm::Error::success();
            };
            for (size_t i = 0; i < inner.size(); ++i) {
                char c = inner[i];
                if (inS) {
                    if (c == '\'') inS = false;
                } else if (inD) {
                    if (c == '\\') { ++i; }
                    else if (c == '"') inD = false;
                } else if (c == '\'') {
                    inS = true;
                } else if (c == '"') {
                    inD = true;
                } else if (c == '[') {
                    ++depth;
                } else if (c == ']') {
                    --depth;
                } else if (c == ',' && depth == 0) {
                    if (auto e = emit(i)) return e;
                    itemStart = i + 1;
                }
            }
            if (auto e = emit(inner.size())) return e;
            return llvm::json::Value(std::move(arr));
        }

        // Type a plain (unquoted) scalar per spec §3.1, after stripping any
        // trailing inline comment (a '#' preceded by whitespace).
        llvm::json::Value typePlainScalar(llvm::StringRef v) {
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

        // Parse a value region (already trimmed on the left): a flow sequence,
        // a quoted string, or a plain typed scalar.
        llvm::Expected<llvm::json::Value> parseScalar(llvm::StringRef v, int lineNo) {
            if (v.empty()) {
                return llvm::json::Value(nullptr);
            }
            switch (v.front()) {
                case '[': return parseFlowSequence(v, lineNo);
                case '\'': return parseSingleQuoted(v, lineNo);
                case '"': return parseDoubleQuoted(v, lineNo);
                default: return typePlainScalar(v);
            }
        }

        // Parse a block of mapping entries all at `indent`. Advances `idx` past
        // every line it consumes (including nested children).
        llvm::Expected<llvm::json::Value>
        parseMapping(const std::vector<Line>& lines, size_t& idx, int indent) {
            llvm::json::Object obj;
            while (idx < lines.size() && lines[idx].indent == indent) {
                const Line& cur = lines[idx];
                if (isSeqItem(cur.text)) {
                    return makeError(cur.lineNo,
                                     "unexpected sequence item in a mapping");
                }
                size_t colon = cur.text.find(':');
                if (colon == llvm::StringRef::npos) {
                    return makeError(cur.lineNo, "expected 'key: value' mapping entry");
                }
                llvm::StringRef keyRef = cur.text.take_front(colon).rtrim();
                if (keyRef.empty()) {
                    return makeError(cur.lineNo, "empty mapping key");
                }
                // json::ObjectKey(StringRef) BORROWS its bytes — store an owned
                // std::string so keys outlive the header buffer they came from.
                std::string key = keyRef.str();
                llvm::StringRef valueRegion = cur.text.drop_front(colon + 1).ltrim();
                ++idx;

                if (valueRegion.empty()) {
                    // Either a nested block (deeper indent) or a null value.
                    if (idx < lines.size() && lines[idx].indent > indent) {
                        auto child = parseBlock(lines, idx, lines[idx].indent);
                        if (!child) return child.takeError();
                        obj[key] = std::move(*child);
                    } else {
                        obj[key] = llvm::json::Value(nullptr);
                    }
                } else {
                    auto val = parseScalar(valueRegion, cur.lineNo);
                    if (!val) return val.takeError();
                    obj[key] = std::move(*val);
                }
            }
            return llvm::json::Value(std::move(obj));
        }

        // Parse a block of sequence entries (`- item`) all at `indent`.
        llvm::Expected<llvm::json::Value>
        parseSequence(const std::vector<Line>& lines, size_t& idx, int indent) {
            llvm::json::Array arr;
            while (idx < lines.size() && lines[idx].indent == indent &&
                   isSeqItem(lines[idx].text)) {
                const Line& cur = lines[idx];
                // Strip the leading "-" and a following space.
                llvm::StringRef item = cur.text.drop_front(1).ltrim();
                ++idx;

                if (item.empty()) {
                    // Nested block on deeper-indented lines, else a null element.
                    if (idx < lines.size() && lines[idx].indent > indent) {
                        auto child = parseBlock(lines, idx, lines[idx].indent);
                        if (!child) return child.takeError();
                        arr.push_back(std::move(*child));
                    } else {
                        arr.push_back(llvm::json::Value(nullptr));
                    }
                } else {
                    auto val = parseScalar(item, cur.lineNo);
                    if (!val) return val.takeError();
                    arr.push_back(std::move(*val));
                }
            }
            return llvm::json::Value(std::move(arr));
        }

        // Dispatch a block at `indent` to a sequence or a mapping based on its
        // first line.
        llvm::Expected<llvm::json::Value>
        parseBlock(const std::vector<Line>& lines, size_t& idx, int indent) {
            if (idx < lines.size() && isSeqItem(lines[idx].text)) {
                return parseSequence(lines, idx, indent);
            }
            return parseMapping(lines, idx, indent);
        }

    } // namespace

    llvm::Expected<llvm::json::Value> parseYaml(std::string_view header, int firstLine) {
        auto lines = scanLines(header, firstLine);
        if (!lines) {
            return lines.takeError();
        }
        if (lines->empty()) {
            return llvm::json::Value(llvm::json::Object{});
        }

        size_t idx = 0;
        int baseIndent = lines->front().indent;
        auto value = parseBlock(*lines, idx, baseIndent);
        if (!value) {
            return value.takeError();
        }
        if (idx < lines->size()) {
            return makeError((*lines)[idx].lineNo, "unexpected indentation");
        }
        return value;
    }

} // namespace cajeta::buildtool
