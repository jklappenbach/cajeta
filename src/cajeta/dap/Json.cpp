#include "cajeta/dap/Json.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace cajeta::dap {

const std::string Json::empty_;
const Json Json::nullSentinel_;

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) { type_ = Type::Object; }
    return obj_[key];
}

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
}

const Json& Json::at(const std::string& key) const {
    if (type_ != Type::Object) return nullSentinel_;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullSentinel_ : it->second;
}

void Json::push_back(Json value) {
    if (type_ != Type::Array) { type_ = Type::Array; }
    arr_.push_back(std::move(value));
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return nullSentinel_;
    return arr_[i];
}

// --- serialization ----------------------------------------------------------

void Json::dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += (bool_ ? "true" : "false"); break;
        case Type::Number: {
            // Integers print without a decimal point; others use %g.
            if (std::isfinite(num_) && num_ == std::floor(num_)
                    && std::fabs(num_) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(num_));
                out += buf;
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
                out += buf;
            }
            break;
        }
        case Type::String: dumpString(str_, out); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& e : arr_) {
                if (!first) out += ',';
                first = false;
                e.dumpTo(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out += ',';
                first = false;
                dumpString(k, out);
                out += ':';
                v.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

// --- parsing ----------------------------------------------------------------

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    explicit Parser(const std::string& str) : s(str) {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                s[i] == '\n' || s[i] == '\r')) i++;
    }

    Json fail() { ok = false; return Json(); }

    Json parseValue() {
        skipWs();
        if (i >= s.size()) return fail();
        char c = s[i];
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return Json(parseString());
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default:  return parseNumber();
        }
    }

    Json parseObject() {
        Json obj = Json::object();
        i++;  // {
        skipWs();
        if (i < s.size() && s[i] == '}') { i++; return obj; }
        while (i < s.size()) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return fail();
            std::string key = parseString();
            if (!ok) return Json();
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail();
            i++;  // :
            Json val = parseValue();
            if (!ok) return Json();
            obj[key] = std::move(val);
            skipWs();
            if (i >= s.size()) return fail();
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return obj; }
            return fail();
        }
        return fail();
    }

    Json parseArray() {
        Json arr = Json::array();
        i++;  // [
        skipWs();
        if (i < s.size() && s[i] == ']') { i++; return arr; }
        while (i < s.size()) {
            Json val = parseValue();
            if (!ok) return Json();
            arr.push_back(std::move(val));
            skipWs();
            if (i >= s.size()) return fail();
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; return arr; }
            return fail();
        }
        return fail();
    }

    std::string parseString() {
        std::string out;
        i++;  // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i >= s.size()) { ok = false; return out; }
                char e = s[i++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) { ok = false; return out; }
                        unsigned code = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = s[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else { ok = false; return out; }
                        }
                        // Encode the BMP code point as UTF-8 (enough for DAP).
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: ok = false; return out;
                }
            } else {
                out += c;
            }
        }
        ok = false;  // unterminated
        return out;
    }

    Json parseBool() {
        if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        return fail();
    }

    Json parseNull() {
        if (s.compare(i, 4, "null") == 0) { i += 4; return Json(nullptr); }
        return fail();
    }

    Json parseNumber() {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        bool any = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') ||
               s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
               s[i] == '+' || s[i] == '-')) { i++; any = true; }
        if (!any) return fail();
        try {
            return Json(std::stod(s.substr(start, i - start)));
        } catch (...) {
            return fail();
        }
    }
};

} // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser p(text);
    Json v = p.parseValue();
    if (p.ok) { p.skipWs(); if (p.i != text.size()) p.ok = false; }
    if (ok) *ok = p.ok;
    return p.ok ? v : Json();
}

} // namespace cajeta::dap
