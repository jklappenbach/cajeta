#include "cajetadoc/DocComment.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace cajetadoc {

namespace {

std::string rtrim(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(0, e);
}

std::string ltrim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

std::string lowerStr(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return o;
}

// Strip the `/**` … `*/` delimiters and the per-line `*` gutter, preserving
// indentation inside fenced code blocks (after the gutter+one-space).
std::vector<std::string> stripGutter(const std::string& raw) {
    std::string inner = raw;
    // remove leading /** and trailing */
    if (inner.size() >= 3 && inner.compare(0, 3, "/**") == 0) inner = inner.substr(3);
    if (inner.size() >= 2 && inner.compare(inner.size() - 2, 2, "*/") == 0) {
        inner = inner.substr(0, inner.size() - 2);
    }
    std::vector<std::string> lines;
    std::istringstream is(inner);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // strip leading whitespace then a single `*` gutter then one space
        std::string t = line;
        size_t b = 0;
        while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
        if (b < t.size() && t[b] == '*') {
            ++b;
            if (b < t.size() && t[b] == ' ') ++b; // one following space
            t = t.substr(b);
        } else {
            t = line; // no gutter — keep as-is (rare)
        }
        lines.push_back(t);
    }
    return lines;
}

// A line begins a block tag if, after trimming leading spaces, it starts with
// `@` followed by a letter. (Inline tags are `{@…}` and never start a line tag.)
bool isBlockTagLine(const std::string& line, std::string& tagName, std::string& rest) {
    std::string t = ltrim(line);
    if (t.size() < 2 || t[0] != '@' || !std::isalpha((unsigned char)t[1])) return false;
    size_t i = 1;
    while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) ++i;
    tagName = t.substr(1, i - 1);
    rest = ltrim(t.substr(i));
    return true;
}

// Extract the first sentence from body text for the summary.
std::string firstSentence(const std::string& body) {
    // first blank line terminates, or sentence-ending punctuation + space/eol
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\n') {
            // blank line?
            if (i + 1 < body.size() && body[i + 1] == '\n') return rtrim(body.substr(0, i));
        }
        if (c == '.' || c == '!' || c == '?') {
            bool eol = (i + 1 >= body.size());
            bool spaceAfter = (i + 1 < body.size() &&
                               (body[i + 1] == ' ' || body[i + 1] == '\n' || body[i + 1] == '\t'));
            // avoid splitting on a `.` inside an obvious abbreviation/number: only
            // split when the preceding char isn't a digit and there's whitespace after.
            bool digitBefore = (i > 0 && std::isdigit((unsigned char)body[i - 1]));
            if ((eol || spaceAfter) && !digitBefore) {
                return rtrim(body.substr(0, i + 1));
            }
        }
    }
    return rtrim(body);
}

// Detect a `{@Summary …}` inline override anywhere in the body (§7.1.7).
bool findSummaryOverride(const std::string& body, std::string& out) {
    const std::string open = "{@Summary";
    size_t p = body.find(open);
    if (p == std::string::npos) {
        // case-insensitive fallback
        std::string lb = lowerStr(body);
        p = lb.find("{@summary");
        if (p == std::string::npos) return false;
    }
    size_t start = p + open.size();
    int depth = 1;
    size_t i = start;
    for (; i < body.size() && depth > 0; ++i) {
        if (body[i] == '{') ++depth;
        else if (body[i] == '}') --depth;
    }
    if (depth != 0) return false; // unterminated
    out = ltrim(rtrim(body.substr(start, (i - 1) - start)));
    return true;
}

} // namespace

bool isDocComment(const std::string& raw) {
    // `/** … */` with at least one content char, and not the empty `/**/`.
    if (raw.size() < 5) return false;
    if (raw.compare(0, 3, "/**") != 0) return false;
    if (raw.compare(raw.size() - 2, 2, "*/") != 0) return false;
    // exclude `/***/` style with nothing? still a doc comment but empty.
    // exclude a plain `/* */` (handled by prefix check) and `/**/`.
    if (raw == "/**/") return false;
    return true;
}

bool tagTakesArg(const std::string& name) {
    std::string n = lowerStr(name);
    return n == "param" || n == "throws" || n == "exception" || n == "serialfield";
}

std::vector<const BlockTag*> DocComment::tags(const std::string& name) const {
    std::string n = lowerStr(name);
    std::vector<const BlockTag*> out;
    for (const auto& t : blockTags) {
        if (lowerStr(t.name) == n) out.push_back(&t);
    }
    return out;
}

DocComment parseDocComment(const std::string& raw) {
    DocComment doc;
    if (!isDocComment(raw)) {
        doc.empty = true;
        return doc;
    }
    std::vector<std::string> lines = stripGutter(raw);

    // Split body lines from the block-tag section. The block-tag section starts
    // at the first line (at line start) that begins a `@tag`; everything from
    // there on is tags.
    size_t firstTag = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string tn, rest;
        if (isBlockTagLine(lines[i], tn, rest)) { firstTag = i; break; }
    }

    std::ostringstream bodyOut;
    for (size_t i = 0; i < firstTag; ++i) {
        bodyOut << lines[i];
        if (i + 1 < firstTag) bodyOut << '\n';
    }
    std::string bodyText = bodyOut.str();
    // trim leading blank lines and the single-line-comment leading space
    // (e.g. `/** A demo. */` -> " A demo." -> "A demo.")
    while (!bodyText.empty() &&
           (bodyText.front() == '\n' || bodyText.front() == ' ' || bodyText.front() == '\t')) {
        bodyText.erase(bodyText.begin());
    }
    bodyText = rtrim(bodyText);

    // summary: override or first sentence
    std::string override;
    if (findSummaryOverride(bodyText, override)) {
        doc.summary = override;
    } else {
        doc.summary = firstSentence(bodyText);
    }
    doc.body = bodyText;

    // parse block tags
    BlockTag* cur = nullptr;
    std::ostringstream curBody;
    auto flush = [&]() {
        if (cur) {
            cur->body = rtrim(curBody.str());
            curBody.str("");
            curBody.clear();
            cur = nullptr;
        }
    };
    for (size_t i = firstTag; i < lines.size(); ++i) {
        std::string tn, rest;
        if (isBlockTagLine(lines[i], tn, rest)) {
            flush();
            doc.blockTags.push_back(BlockTag{});
            cur = &doc.blockTags.back();
            cur->name = tn;
            if (tagTakesArg(tn)) {
                // split first token as arg
                std::string r = ltrim(rest);
                size_t sp = 0;
                while (sp < r.size() && !std::isspace((unsigned char)r[sp])) ++sp;
                cur->arg = r.substr(0, sp);
                curBody << ltrim(r.substr(sp));
            } else {
                curBody << rest;
            }
        } else if (cur) {
            curBody << '\n' << lines[i];
        }
    }
    flush();

    // diagnostics: unterminated inline tag in body (unbalanced `{@`)
    {
        size_t pos = 0;
        while ((pos = doc.body.find("{@", pos)) != std::string::npos) {
            int depth = 1;
            size_t j = pos + 2;
            for (; j < doc.body.size() && depth > 0; ++j) {
                if (doc.body[j] == '{') ++depth;
                else if (doc.body[j] == '}') --depth;
            }
            if (depth != 0) {
                doc.diagnostics.push_back(
                    DocDiagnostic{"unterminated-inline-tag",
                                  "unterminated inline tag starting at offset " +
                                      std::to_string(pos), 0});
                break;
            }
            pos = j;
        }
    }

    if (doc.body.empty() && doc.blockTags.empty()) doc.empty = true;
    return doc;
}

} // namespace cajetadoc
