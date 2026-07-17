//
// Shared annotation-instance parsing (REFL-6b). See AnnotationParser.h.
// Moved verbatim from CajetaLlvmVisitor's private statics so both the
// class-body walk and FormalParameter::fromContext build identical
// AnnotationInstances (names + argument values).
//

#include "AnnotationParser.h"

#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "cajeta/type/CajetaType.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    // ide-symbol-index: record the annotation NAME (`@Retry`) as an xref type
    // reference at its token, so Ctrl-click on an annotation navigates to its
    // declaration. Like allocation-created types, the annotation name is
    // resolved off the parse-time type path (it goes through QualifiedName, not
    // CajetaType::fromContext), so without this it carries no edge. Gated on
    // xref::captureEnabled(), so a normal compile never runs it; a miss records
    // nothing.
    static void recordAnnotationXref(const QualifiedNamePtr& qn,
                                     antlr4::Token* tok) {
        if (!xref::captureEnabled() || !qn || !tok || !tok->getInputStream())
            return;
        const std::string* file =
            xref::internSourceFile(tok->getInputStream()->getSourceName());
        if (!file) return;
        try {
            // A bare `@Ann` canonicalizes to `code.Ann` (QualifiedName::
            // fromContext defaults the package to "code"), exactly as the
            // annotation's declaration registers — so the qn's canonical IS
            // the target. Record only when it names a REAL declaration: a
            // compiler intrinsic like @Native / @Inline has none, and an edge
            // to a non-existent declaration is just noise (pruned anyway).
            std::string target = qn->toCanonical();
            auto lt = target.find('<');
            if (lt != std::string::npos) target = target.substr(0, lt);
            if (target.empty()) return;
            auto& cm = CajetaType::getCanonicalMap();
            if (cm.find(target) == cm.end()) return;
            xref::noteTypeReference(target, *file, (int) tok->getLine(),
                                    (int) tok->getCharPositionInLine());
        } catch (...) {}
    }

    // Strip surrounding ASCII whitespace from a literal's text. Annotation
    // argument tokens come from ANTLR's getText() which concatenates token text
    // verbatim — array initializers in particular contain interior whitespace
    // around the commas.
    static std::string trimWs(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char) s[b])) ++b;
        while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    // Classify a single element-value token text and write the discriminated
    // result into `out`. Recognized shapes:
    //   "foo"     → String,   strVal=foo
    //   123       → Int64,    i64Val=123
    //   true      → Bool,     boolVal=true
    //   Foo.class → ClassRef, strVal=Foo
    // Anything else falls through to String with the raw text. Returns true on
    // a confident classification.
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
        // Class literal — `Foo.class`. Strip the suffix and capture the
        // type-name prefix; pointcut matching resolves it against the
        // registered classes. Qualified names (`pkg.Foo.class`) keep the dots.
        {
            static const std::string suffix = ".class";
            if (t.size() > suffix.size()
                    && std::equal(suffix.rbegin(), suffix.rend(), t.rbegin())) {
                out.kind = AnnotationArgKind::ClassRef;
                out.strVal = t.substr(0, t.size() - suffix.size());
                return true;
            }
        }
        // Integer (decimal, with optional leading sign).
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
                // Fall through to raw-string fallback.
            }
        }
        // Unknown shape (identifier reference, enum constant, etc.). Keep as a
        // raw string so consumers see the source text.
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
            // `pkg.@MyAnn` form — leaf identifier is the annotation name.
            const auto& ids = alt->identifier();
            if (!ids.empty()) {
                qn = QualifiedName::getOrInsert(ids.back()->getText(), "");
                nameTok = ids.back()->getStart();
            }
        }
        if (!qn) return nullptr;
        recordAnnotationXref(qn, nameTok);

        auto inst = std::make_shared<AnnotationInstance>(qn);

        // Populate a single AnnotationArg from one elementValue context.
        // Recursively handles array initializers by collecting child texts.
        std::function<void(CajetaParser::ElementValueContext*, AnnotationArg&)> readArg =
            [&](CajetaParser::ElementValueContext* ev, AnnotationArg& arg) {
                if (!ev) return;
                if (auto* arr = ev->elementValueArrayInitializer()) {
                    // Array — classify each child, then pick the dominant kind.
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
                            // Homogeneous string array → unquoted payload;
                            // mixed shapes stringify ints/bools / keep strVal.
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
                    // Nested annotation — captured as raw text for now.
                    arg.kind = AnnotationArgKind::String;
                    arg.strVal = nested->getText();
                    return;
                }
                // expression — text-classify the leaf token.
                classifyLiteral(ev->getText(), arg);
            };

        if (auto* evp = ann->elementValuePairs()) {
            // `@Foo(name = value, other = thing)`.
            for (auto* pair : evp->elementValuePair()) {
                AnnotationArg arg;
                if (pair->identifier()) {
                    arg.name = pair->identifier()->getText();
                }
                readArg(pair->elementValue(), arg);
                inst->addArg(std::move(arg));
            }
        } else if (auto* ev = ann->elementValue()) {
            // `@Foo(value)` — single unnamed arg. Stored with empty name;
            // findArg("value") routes through to it.
            AnnotationArg arg;
            readArg(ev, arg);
            inst->addArg(std::move(arg));
        }
        // `@Foo` with no parens — empty args; the instance still records the
        // annotation by name.
        return inst;
    }

} // namespace cajeta
