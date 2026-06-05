#include "cajetadoc/Markdown.h"

#include <cctype>
#include <sstream>
#include <vector>

namespace cajetadoc {

std::string htmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Expand a JavaDoc inline tag body that starts just after "{@Name ". Returns
// HTML. `name` is lowercased.
std::string expandInlineTag(const std::string& name, const std::string& body,
                            const MarkdownOptions& opts) {
    if (name == "code" || name == "literal") {
        // verbatim, escaped, no markdown
        std::string inner = htmlEscape(body);
        return name == "code" ? "<code>" + inner + "</code>" : inner;
    }
    if (name == "link" || name == "linkplain" || name == "value") {
        std::string target = trim(body);
        // optional label after whitespace
        std::string label = target;
        size_t sp = target.find_first_of(" \t");
        if (sp != std::string::npos) {
            label = trim(target.substr(sp));
            target = target.substr(0, sp);
        }
        std::string href;
        if (opts.linkResolver) href = opts.linkResolver(target);
        if (!href.empty()) {
            if (name == "linkplain")
                return "<a href=\"" + htmlEscape(href) + "\">" + htmlEscape(label) + "</a>";
            return "<a href=\"" + htmlEscape(href) + "\"><code>" + htmlEscape(label) +
                   "</code></a>";
        }
        if (name == "linkplain") return htmlEscape(label);
        return "<code>" + htmlEscape(label) + "</code>";
    }
    if (name == "index" || name == "summary" || name == "systemproperty") {
        return htmlEscape(trim(body));
    }
    if (name == "docroot") return "."; // resolved more precisely by the page emitter
    // unknown inline tag: render its body escaped
    return htmlEscape(trim(body));
}

// Inline rendering: handles `code`, **strong**, *em*, [text](url), images,
// JavaDoc inline tags {@...}, and auto-escapes everything else.
std::string inlineRender(const std::string& src, const MarkdownOptions& opts) {
    std::string out;
    size_t i = 0;
    while (i < src.size()) {
        char c = src[i];
        // inline tag {@Name ...}
        if (c == '{' && i + 1 < src.size() && src[i + 1] == '@') {
            size_t j = i + 2;
            std::string name;
            while (j < src.size() && (std::isalnum((unsigned char)src[j]))) {
                name += (char)std::tolower((unsigned char)src[j]);
                ++j;
            }
            // skip one space
            if (j < src.size() && src[j] == ' ') ++j;
            int depth = 1;
            std::string body;
            for (; j < src.size() && depth > 0; ++j) {
                if (src[j] == '{') { ++depth; body += '{'; }
                else if (src[j] == '}') { --depth; if (depth > 0) body += '}'; }
                else body += src[j];
            }
            out += expandInlineTag(name, body, opts);
            i = j;
            continue;
        }
        // inline code `...`
        if (c == '`') {
            size_t j = i + 1;
            while (j < src.size() && src[j] != '`') ++j;
            if (j < src.size()) {
                out += "<code>" + htmlEscape(src.substr(i + 1, j - i - 1)) + "</code>";
                i = j + 1;
                continue;
            }
        }
        // image ![alt](url)
        if (c == '!' && i + 1 < src.size() && src[i + 1] == '[') {
            size_t close = src.find(']', i + 2);
            if (close != std::string::npos && close + 1 < src.size() && src[close + 1] == '(') {
                size_t paren = src.find(')', close + 2);
                if (paren != std::string::npos) {
                    std::string alt = src.substr(i + 2, close - (i + 2));
                    std::string url = src.substr(close + 2, paren - (close + 2));
                    out += "<img src=\"" + htmlEscape(url) + "\" alt=\"" + htmlEscape(alt) + "\">";
                    i = paren + 1;
                    continue;
                }
            }
        }
        // link [text](url)
        if (c == '[') {
            size_t close = src.find(']', i + 1);
            if (close != std::string::npos && close + 1 < src.size() && src[close + 1] == '(') {
                size_t paren = src.find(')', close + 2);
                if (paren != std::string::npos) {
                    std::string text = src.substr(i + 1, close - (i + 1));
                    std::string url = src.substr(close + 2, paren - (close + 2));
                    out += "<a href=\"" + htmlEscape(url) + "\">" + inlineRender(text, opts) +
                           "</a>";
                    i = paren + 1;
                    continue;
                }
            }
            // bracket cross-ref [Target] (no parens) -> resolver
            if (close != std::string::npos &&
                (close + 1 >= src.size() || src[close + 1] != '(')) {
                std::string target = src.substr(i + 1, close - (i + 1));
                std::string href;
                if (opts.linkResolver && target.find(' ') == std::string::npos)
                    href = opts.linkResolver(target);
                if (!href.empty()) {
                    out += "<a href=\"" + htmlEscape(href) + "\"><code>" + htmlEscape(target) +
                           "</code></a>";
                    i = close + 1;
                    continue;
                }
            }
        }
        // strong **...**
        if (c == '*' && i + 1 < src.size() && src[i + 1] == '*') {
            size_t j = src.find("**", i + 2);
            if (j != std::string::npos) {
                out += "<strong>" + inlineRender(src.substr(i + 2, j - i - 2), opts) +
                       "</strong>";
                i = j + 2;
                continue;
            }
        }
        // emphasis *...* or _..._
        if ((c == '*' || c == '_')) {
            size_t j = src.find(c, i + 1);
            if (j != std::string::npos && j > i + 1) {
                out += "<em>" + inlineRender(src.substr(i + 1, j - i - 1), opts) + "</em>";
                i = j + 1;
                continue;
            }
        }
        // escape the literal char
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
        ++i;
    }
    return out;
}

bool isTableSeparator(const std::string& line) {
    std::string t = trim(line);
    if (t.find('|') == std::string::npos && t.find('-') == std::string::npos) return false;
    bool sawDash = false;
    for (char c : t) {
        if (c == '-') sawDash = true;
        else if (c != '|' && c != ':' && c != ' ') return false;
    }
    return sawDash;
}

std::vector<std::string> splitCells(const std::string& row) {
    std::string t = trim(row);
    if (!t.empty() && t.front() == '|') t.erase(t.begin());
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += '|'; ++i; continue; }
        if (t[i] == '|') { cells.push_back(trim(cur)); cur.clear(); }
        else cur += t[i];
    }
    cells.push_back(trim(cur));
    return cells;
}

} // namespace

std::string renderInline(const std::string& md, const MarkdownOptions& opts) {
    return inlineRender(md, opts);
}

std::string renderMarkdown(const std::string& md, const MarkdownOptions& opts) {
    std::vector<std::string> lines;
    {
        std::istringstream is(md);
        std::string l;
        while (std::getline(is, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }

    std::ostringstream out;
    size_t i = 0;
    auto isBlank = [&](size_t k) { return k >= lines.size() || trim(lines[k]).empty(); };

    while (i < lines.size()) {
        const std::string& line = lines[i];
        std::string t = trim(line);

        if (t.empty()) { ++i; continue; }

        // fenced code block
        if (t.compare(0, 3, "```") == 0) {
            std::string lang = trim(t.substr(3));
            out << "<pre><code";
            if (!lang.empty()) out << " class=\"language-" << htmlEscape(lang) << "\"";
            out << ">";
            ++i;
            std::ostringstream code;
            while (i < lines.size() && trim(lines[i]).compare(0, 3, "```") != 0) {
                code << lines[i] << "\n";
                ++i;
            }
            if (i < lines.size()) ++i; // closing fence
            out << htmlEscape(code.str());
            out << "</code></pre>\n";
            continue;
        }

        // ATX heading
        if (t[0] == '#') {
            int level = 0;
            while (level < (int)t.size() && t[level] == '#') ++level;
            if (level <= 6 && level < (int)t.size() && t[level] == ' ') {
                int h = level + opts.headingOffset;
                if (h > 6) h = 6;
                std::string text = trim(t.substr(level));
                out << "<h" << h << ">" << inlineRender(text, opts) << "</h" << h << ">\n";
                ++i;
                continue;
            }
        }

        // thematic break
        if (t == "---" || t == "***" || t == "___") {
            out << "<hr>\n";
            ++i;
            continue;
        }

        // GFM table: header row, then separator
        if (t.find('|') != std::string::npos && i + 1 < lines.size() &&
            isTableSeparator(lines[i + 1])) {
            std::vector<std::string> header = splitCells(lines[i]);
            i += 2;
            out << "<table>\n<thead>\n<tr>";
            for (auto& c : header) out << "<th>" << inlineRender(c, opts) << "</th>";
            out << "</tr>\n</thead>\n<tbody>\n";
            while (i < lines.size() && trim(lines[i]).find('|') != std::string::npos &&
                   !trim(lines[i]).empty()) {
                std::vector<std::string> cells = splitCells(lines[i]);
                out << "<tr>";
                for (auto& c : cells) out << "<td>" << inlineRender(c, opts) << "</td>";
                out << "</tr>\n";
                ++i;
            }
            out << "</tbody>\n</table>\n";
            continue;
        }

        // blockquote
        if (t[0] == '>') {
            std::ostringstream inner;
            while (i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '>') {
                std::string q = trim(lines[i]);
                q.erase(q.begin()); // '>'
                if (!q.empty() && q[0] == ' ') q.erase(q.begin());
                inner << q << "\n";
                ++i;
            }
            out << "<blockquote>\n" << renderMarkdown(inner.str(), opts) << "</blockquote>\n";
            continue;
        }

        // unordered list
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            out << "<ul>\n";
            while (i < lines.size()) {
                std::string lt = trim(lines[i]);
                if (lt.size() > 1 && (lt[0] == '-' || lt[0] == '*' || lt[0] == '+') &&
                    lt[1] == ' ') {
                    std::ostringstream item;
                    item << lt.substr(2);
                    ++i;
                    // continuation / nested lines (indented)
                    while (i < lines.size() && !isBlank(i) && (lines[i].size() > 0) &&
                           (lines[i][0] == ' ' || lines[i][0] == '\t')) {
                        item << "\n" << trim(lines[i]);
                        ++i;
                    }
                    out << "<li>" << inlineRender(trim(item.str()), opts) << "</li>\n";
                } else {
                    break;
                }
            }
            out << "</ul>\n";
            continue;
        }

        // ordered list
        if (std::isdigit((unsigned char)t[0])) {
            size_t d = 0;
            while (d < t.size() && std::isdigit((unsigned char)t[d])) ++d;
            if (d < t.size() && (t[d] == '.' || t[d] == ')') && d + 1 < t.size() &&
                t[d + 1] == ' ') {
                out << "<ol>\n";
                while (i < lines.size()) {
                    std::string lt = trim(lines[i]);
                    size_t dd = 0;
                    while (dd < lt.size() && std::isdigit((unsigned char)lt[dd])) ++dd;
                    if (dd > 0 && dd < lt.size() && (lt[dd] == '.' || lt[dd] == ')') &&
                        dd + 1 < lt.size() && lt[dd + 1] == ' ') {
                        out << "<li>" << inlineRender(trim(lt.substr(dd + 2)), opts) << "</li>\n";
                        ++i;
                    } else {
                        break;
                    }
                }
                out << "</ol>\n";
                continue;
            }
        }

        // paragraph: gather until blank line or a block starter
        std::ostringstream para;
        bool first = true;
        while (i < lines.size() && !isBlank(i)) {
            std::string lt = trim(lines[i]);
            if (lt.compare(0, 3, "```") == 0 || lt[0] == '#' || lt[0] == '>' ||
                ((lt[0] == '-' || lt[0] == '*' || lt[0] == '+') && lt.size() > 1 && lt[1] == ' '))
                break;
            if (!first) para << " ";
            para << lt;
            first = false;
            ++i;
        }
        out << "<p>" << inlineRender(para.str(), opts) << "</p>\n";
    }

    return out.str();
}

} // namespace cajetadoc
