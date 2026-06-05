#include "cajetadoc/Json.h"

#include <algorithm>
#include <sstream>

#include "cajetadoc/DocComment.h"
#include "cajetadoc/Model.h"

namespace cajetadoc {

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    o += "\\u00";
                    o += hex[((unsigned char)c >> 4) & 0xF];
                    o += hex[(unsigned char)c & 0xF];
                } else {
                    o += c;
                }
        }
    }
    return o;
}

namespace {

struct W {
    std::ostringstream os;
    int indent = 0;
    void pad() { for (int i = 0; i < indent; ++i) os << "  "; }
    void str(const std::string& k, const std::string& v) {
        pad();
        os << '"' << jsonEscape(k) << "\": \"" << jsonEscape(v) << "\"";
    }
};

void writeStringArray(W& w, const std::string& key, const std::vector<std::string>& v) {
    w.pad();
    w.os << '"' << key << "\": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) w.os << ", ";
        w.os << '"' << jsonEscape(v[i]) << '"';
    }
    w.os << "]";
}

void writeDoc(W& w, const DocComment* doc) {
    w.pad();
    if (!doc) { w.os << "\"doc\": null"; return; }
    w.os << "\"doc\": {\n";
    w.indent++;
    w.str("summary", doc->summary);
    w.os << ",\n";
    w.pad();
    w.os << "\"blockTags\": [";
    for (size_t i = 0; i < doc->blockTags.size(); ++i) {
        const auto& t = doc->blockTags[i];
        if (i) w.os << ",";
        w.os << "\n";
        w.indent++;
        w.pad();
        w.os << "{\"name\": \"" << jsonEscape(t.name) << "\", \"arg\": \""
             << jsonEscape(t.arg) << "\", \"body\": \"" << jsonEscape(t.body) << "\"}";
        w.indent--;
    }
    if (!doc->blockTags.empty()) { w.os << "\n"; w.pad(); }
    w.os << "]";
    w.indent--;
    w.os << "\n";
    w.pad();
    w.os << "}";
}

void writeParam(W& w, const Param& p) {
    w.pad();
    w.os << "{\"name\": \"" << jsonEscape(p.name) << "\", \"type\": \"" << jsonEscape(p.type)
         << "\", \"transfer\": " << (p.ownershipTransfer ? "true" : "false")
         << ", \"variadic\": " << (p.variadic ? "true" : "false") << "}";
}

void writeMember(W& w, const Member& m) {
    w.pad();
    w.os << "{\n";
    w.indent++;
    w.str("kind", toString(m.kind));
    w.os << ",\n";
    w.str("name", m.name);
    w.os << ",\n";
    w.str("visibility", toString(m.visibility));
    w.os << ",\n";
    w.str("signature", m.signature());
    w.os << ",\n";
    std::vector<std::string> mods(m.modifiers.begin(), m.modifiers.end());
    std::sort(mods.begin(), mods.end());
    writeStringArray(w, "modifiers", mods);
    w.os << ",\n";
    writeStringArray(w, "annotations", m.annotations);
    w.os << ",\n";
    w.pad();
    w.os << "\"params\": [";
    for (size_t i = 0; i < m.params.size(); ++i) {
        if (i) w.os << ",";
        w.os << "\n";
        w.indent++;
        writeParam(w, m.params[i]);
        w.indent--;
    }
    if (!m.params.empty()) { w.os << "\n"; w.pad(); }
    w.os << "],\n";
    writeStringArray(w, "throws", m.throws);
    w.os << ",\n";
    writeDoc(w, m.doc.get());
    w.os << "\n";
    w.indent--;
    w.pad();
    w.os << "}";
}

void writeType(W& w, const Type& t) {
    w.pad();
    w.os << "{\n";
    w.indent++;
    w.str("kind", toString(t.kind));
    w.os << ",\n";
    w.str("name", t.name);
    w.os << ",\n";
    w.str("qualifiedName", t.qualifiedName());
    w.os << ",\n";
    w.str("visibility", toString(t.visibility));
    w.os << ",\n";
    writeStringArray(w, "extends", t.extends);
    w.os << ",\n";
    writeStringArray(w, "implements", t.implements);
    w.os << ",\n";
    writeStringArray(w, "annotations", t.annotations);
    w.os << ",\n";
    w.pad();
    w.os << "\"members\": [";
    for (size_t i = 0; i < t.members.size(); ++i) {
        if (i) w.os << ",";
        w.os << "\n";
        w.indent++;
        writeMember(w, t.members[i]);
        w.indent--;
    }
    if (!t.members.empty()) { w.os << "\n"; w.pad(); }
    w.os << "],\n";
    w.pad();
    w.os << "\"nested\": [";
    for (size_t i = 0; i < t.nested.size(); ++i) {
        if (i) w.os << ",";
        w.os << "\n";
        w.indent++;
        writeType(w, t.nested[i]);
        w.indent--;
    }
    if (!t.nested.empty()) { w.os << "\n"; w.pad(); }
    w.os << "],\n";
    writeDoc(w, t.doc.get());
    w.os << "\n";
    w.indent--;
    w.pad();
    w.os << "}";
}

} // namespace

std::string toJson(const Model& model) {
    // sort packages by name for determinism
    std::vector<const Package*> pkgs;
    for (const auto& p : model.packages) pkgs.push_back(&p);
    std::sort(pkgs.begin(), pkgs.end(),
              [](const Package* a, const Package* b) { return a->name < b->name; });

    W w;
    w.os << "{\n";
    w.indent++;
    w.pad();
    w.os << "\"packages\": [";
    for (size_t i = 0; i < pkgs.size(); ++i) {
        const Package* p = pkgs[i];
        if (i) w.os << ",";
        w.os << "\n";
        w.indent++;
        w.pad();
        w.os << "{\n";
        w.indent++;
        w.str("name", p->name);
        w.os << ",\n";
        w.pad();
        w.os << "\"types\": [";
        for (size_t j = 0; j < p->types.size(); ++j) {
            if (j) w.os << ",";
            w.os << "\n";
            w.indent++;
            writeType(w, p->types[j]);
            w.indent--;
        }
        if (!p->types.empty()) { w.os << "\n"; w.pad(); }
        w.os << "]\n";
        w.indent--;
        w.pad();
        w.os << "}";
        w.indent--;
    }
    if (!pkgs.empty()) { w.os << "\n"; w.pad(); }
    w.os << "]\n";
    w.indent--;
    w.os << "}\n";
    return w.os.str();
}

std::string toJson(const DocComment& doc) {
    W w;
    writeDoc(w, &doc);
    return w.os.str() + "\n";
}

} // namespace cajetadoc
