// Shared annotation-instance parsing. See AnnotationParser.h.

#include "AnnotationParser.h"

#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    // Record the annotation NAME as an xref type reference at its token, so Ctrl-click
    // navigates to it; the parse-time type path carries no such edge on its own.
    static void recordAnnotationXref(const QualifiedNamePtr& qn,
                                     antlr4::Token* tok) {
        if (!xref::captureEnabled() || !qn || !tok || !tok->getInputStream())
            return;
        const std::string* file =
            xref::internSourceFile(tok->getInputStream()->getSourceName());
        if (!file) return;
        try {
            // The registration KEY is collision-safe "code.<Name>", but the real qName is
            // the navigable identity, so emit that. A compiler intrinsic yields no edge.
            QualifiedNamePtr key =
                QualifiedName::getOrInsert(qn->getTypeName(), "code");
            auto& cm = CajetaType::getCanonicalMap();
            auto it = cm.find(key->toCanonical());
            if (it == cm.end() || !it->second) return;
            auto klass = std::dynamic_pointer_cast<CajetaClass>(it->second);
            if (!klass || !klass->isAnnotation() || !klass->getQName()) return;
            std::string target = klass->getQName()->toCanonical();
            if (target.empty()) return;
            xref::noteTypeReference(target, *file, (int) tok->getLine(),
                                    (int) tok->getCharPositionInLine());
        } catch (...) {}
    }

    // Strip surrounding ASCII whitespace from a literal's text; ANTLR's getText()
    // concatenates token text verbatim, so array initializers carry interior spaces.
    static std::string trimWs(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char) s[b])) ++b;
        while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    // Classify one element-value token text into `out`: "foo" is a String, 123 an
    // Int64, true a Bool, Foo.class a ClassRef. Anything else falls back to String
    // with the raw text. Returns true only on a confident classification.
    static bool classifyLiteral(const std::string& raw, AnnotationArg& out) {
        std::string t = trimWs(raw);
        if (t.empty()) {
            out.kind = AnnotationArgKind::String;
            out.strVal = "";
            return false;
        }
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            out.kind = AnnotationArgKind::String;
            out.strVal = t.substr(1, t.size() - 2);
            return true;
        }
        if (t == "true" || t == "false") {
            out.kind = AnnotationArgKind::Bool;
            out.boolVal = (t == "true");
            return true;
        }
        // Strip the `.class` suffix; a qualified name keeps its dots, and pointcut
        // matching resolves the prefix against the registered classes.
        {
            static const std::string suffix = ".class";
            if (t.size() > suffix.size()
                    && std::equal(suffix.rbegin(), suffix.rend(), t.rbegin())) {
                out.kind = AnnotationArgKind::ClassRef;
                out.strVal = t.substr(0, t.size() - suffix.size());
                return true;
            }
        }
        bool numeric = !t.empty();
        size_t i = 0;
        if (t[0] == '+' || t[0] == '-') ++i;
        if (i == t.size()) numeric = false;
        for (; i < t.size() && numeric; ++i) {
            if (!std::isdigit((unsigned char) t[i])) numeric = false;
        }
        if (numeric) {
            try {
                out.kind = AnnotationArgKind::Int64;
                out.i64Val = (int64_t) std::stoll(t);
                return true;
            } catch (...) {
            }
        }
        // Unknown shape: keep the raw source text so consumers still see it.
        out.kind = AnnotationArgKind::String;
        out.strVal = t;
        return false;
    }

    AnnotationInstancePtr parseAnnotationInstance(CajetaParser::AnnotationContext* ann) {
        if (!ann) return nullptr;
        QualifiedNamePtr qn;
        antlr4::Token* nameTok = nullptr;   // the annotation type-name token
        if (ann->qualifiedName()) {
            qn = QualifiedName::fromContext(ann->qualifiedName());
            const auto& ids = ann->qualifiedName()->identifier();
            if (!ids.empty()) nameTok = ids.back()->getStart();
        } else if (auto* alt = ann->altAnnotationQualifiedName()) {
            const auto& ids = alt->identifier();
            if (!ids.empty()) {
                qn = QualifiedName::getOrInsert(ids.back()->getText(), "");
                nameTok = ids.back()->getStart();
            }
        }
        if (!qn) return nullptr;
        recordAnnotationXref(qn, nameTok);

        auto inst = std::make_shared<AnnotationInstance>(qn);

        // Populate one AnnotationArg from an elementValue, recursing into array
        // initializers by collecting the child texts.
        std::function<void(CajetaParser::ElementValueContext*, AnnotationArg&)> readArg =
            [&](CajetaParser::ElementValueContext* ev, AnnotationArg& arg) {
                if (!ev) return;
                if (auto* arr = ev->elementValueArrayInitializer()) {
                    std::vector<AnnotationArg> parts;
                    for (auto* child : arr->elementValue()) {
                        AnnotationArg p;
                        readArg(child, p);
                        parts.push_back(std::move(p));
                    }
                    bool allInt = !parts.empty(), allBool = !parts.empty();
                    for (auto& p : parts) {
                        if (p.kind != AnnotationArgKind::Int64) allInt = false;
                        if (p.kind != AnnotationArgKind::Bool)  allBool = false;
                    }
                    if (allInt) {
                        arg.kind = AnnotationArgKind::Int64List;
                        for (auto& p : parts) arg.i64List.push_back(p.i64Val);
                    } else if (allBool) {
                        arg.kind = AnnotationArgKind::BoolList;
                        for (auto& p : parts) arg.boolList.push_back(p.boolVal);
                    } else {
                        arg.kind = AnnotationArgKind::StringList;
                        for (auto& p : parts) {
                            // A homogeneous string array carries an unquoted payload.
                            if (p.kind == AnnotationArgKind::String) {
                                arg.strList.push_back(p.strVal);
                            } else if (p.kind == AnnotationArgKind::Int64) {
                                arg.strList.push_back(std::to_string(p.i64Val));
                            } else if (p.kind == AnnotationArgKind::Bool) {
                                arg.strList.push_back(p.boolVal ? "true" : "false");
                            } else {
                                arg.strList.push_back(p.strVal);
                            }
                        }
                    }
                    return;
                }
                if (auto* nested = ev->annotation()) {
                    arg.kind = AnnotationArgKind::String;
                    arg.strVal = nested->getText();
                    return;
                }
                classifyLiteral(ev->getText(), arg);
            };

        if (auto* evp = ann->elementValuePairs()) {
            for (auto* pair : evp->elementValuePair()) {
                AnnotationArg arg;
                if (pair->identifier()) {
                    arg.name = pair->identifier()->getText();
                }
                readArg(pair->elementValue(), arg);
                inst->addArg(std::move(arg));
            }
        } else if (auto* ev = ann->elementValue()) {
            // Single unnamed arg: stored with an empty name, and findArg("value") routes to it.
            AnnotationArg arg;
            readArg(ev, arg);
            inst->addArg(std::move(arg));
        }
        return inst;
    }

} // namespace cajeta
