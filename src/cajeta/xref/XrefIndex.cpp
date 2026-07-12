#include "cajeta/xref/XrefIndex.h"

#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/StructureProperty.h"
#include "cajeta/method/Method.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace cajeta::xref {

    namespace {

        void escapeInto(std::ostringstream& out, const std::string& s) {
            for (char c : s) {
                switch (c) {
                    case '"':  out << "\\\""; break;
                    case '\\': out << "\\\\"; break;
                    case '\n': out << "\\n";  break;
                    case '\r': out << "\\r";  break;
                    case '\t': out << "\\t";  break;
                    default:
                        if ((unsigned char) c < 0x20) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char) c);
                            out << buf;
                        } else {
                            out << c;
                        }
                }
            }
        }

        void field(std::ostringstream& out, const char* key, const std::string& val,
                   bool& first) {
            if (val.empty()) return;          // omit empties — keeps the doc small
            if (!first) out << ", ";
            out << "\"" << key << "\": \"";
            escapeInto(out, val);
            out << "\"";
            first = false;
        }

        void field(std::ostringstream& out, const char* key, int val, bool& first) {
            if (!first) out << ", ";
            out << "\"" << key << "\": " << val;
            first = false;
        }

        void writeSourceRef(std::ostringstream& out, const SourceRef& at, bool& first) {
            field(out, "file", at.file, first);
            if (at.line > 0) {
                field(out, "line", at.line, first);
                field(out, "col", at.col, first);
            }
        }

        // Deterministic ordering: by position, then by identity. Two records that
        // sort equal are duplicates and get collapsed.
        bool refLess(const SourceRef& a, const SourceRef& b) {
            if (a.file != b.file) return a.file < b.file;
            if (a.line != b.line) return a.line < b.line;
            return a.col < b.col;
        }

        bool declLess(const Declaration& a, const Declaration& b) {
            if (a.fqn != b.fqn) return a.fqn < b.fqn;
            if (a.overloadKey != b.overloadKey) return a.overloadKey < b.overloadKey;
            return refLess(a.at, b.at);
        }
        bool declEq(const Declaration& a, const Declaration& b) {
            return a.fqn == b.fqn && a.overloadKey == b.overloadKey
                && a.at.file == b.at.file && a.at.line == b.at.line
                && a.at.col == b.at.col;
        }

        bool edgeLess(const InheritanceEdge& a, const InheritanceEdge& b) {
            if (a.child != b.child) return a.child < b.child;
            if (a.kind != b.kind) return a.kind < b.kind;
            return a.parent < b.parent;
        }
        bool edgeEq(const InheritanceEdge& a, const InheritanceEdge& b) {
            return a.child == b.child && a.parent == b.parent && a.kind == b.kind;
        }

        bool refLess2(const Reference& a, const Reference& b) {
            if (!(a.at.file == b.at.file && a.at.line == b.at.line
                    && a.at.col == b.at.col))
                return refLess(a.at, b.at);
            if (a.target != b.target) return a.target < b.target;
            return a.kind < b.kind;
        }
        bool refEq2(const Reference& a, const Reference& b) {
            return a.target == b.target && a.kind == b.kind
                && a.at.file == b.at.file && a.at.line == b.at.line
                && a.at.col == b.at.col;
        }

        bool ovLess(const OverrideEdge& a, const OverrideEdge& b) {
            if (a.method != b.method) return a.method < b.method;
            return a.overrides < b.overrides;
        }
        bool ovEq(const OverrideEdge& a, const OverrideEdge& b) {
            return a.method == b.method && a.overrides == b.overrides;
        }

        bool callLess(const Call& a, const Call& b) {
            if (!(a.at.file == b.at.file && a.at.line == b.at.line
                    && a.at.col == b.at.col))
                return refLess(a.at, b.at);
            return a.callee < b.callee;
        }
        bool callEq(const Call& a, const Call& b) {
            return a.callee == b.callee && a.caller == b.caller
                && a.at.file == b.at.file && a.at.line == b.at.line
                && a.at.col == b.at.col;
        }

        template <typename T, typename Less, typename Eq>
        std::vector<T> sortedUnique(std::vector<T> v, Less less, Eq eq) {
            std::sort(v.begin(), v.end(), less);
            v.erase(std::unique(v.begin(), v.end(), eq), v.end());
            return v;
        }

    } // namespace

    std::string XrefIndex::toJson() const {
        auto decls  = sortedUnique(declarations_, declLess, declEq);
        auto inh    = sortedUnique(inheritance_, edgeLess, edgeEq);
        auto refs   = sortedUnique(references_, refLess2, refEq2);
        auto ovs    = sortedUnique(overrides_, ovLess, ovEq);
        auto calls  = sortedUnique(calls_, callLess, callEq);

        std::ostringstream out;
        out << "{\n";
        out << "  \"version\": {\"major\": " << kSchemaMajor
            << ", \"minor\": " << kSchemaMinor << "},\n";
        if (!sourceRoot_.empty()) {
            out << "  \"sourceRoot\": \"";
            escapeInto(out, sourceRoot_);
            out << "\",\n";
        }

        out << "  \"declarations\": [";
        for (size_t i = 0; i < decls.size(); ++i) {
            const auto& d = decls[i];
            out << (i ? ",\n    " : "\n    ") << "{";
            bool first = true;
            field(out, "fqn", d.fqn, first);
            field(out, "kind", d.kind, first);
            writeSourceRef(out, d.at, first);
            field(out, "owner", d.owner, first);
            field(out, "signature", d.signature, first);
            field(out, "overloadKey", d.overloadKey, first);
            if (!d.modifiers.empty()) {
                if (!first) out << ", ";
                out << "\"modifiers\": [";
                for (size_t m = 0; m < d.modifiers.size(); ++m) {
                    if (m) out << ", ";
                    out << "\"";
                    escapeInto(out, d.modifiers[m]);
                    out << "\"";
                }
                out << "]";
            }
            out << "}";
        }
        out << (decls.empty() ? "" : "\n  ") << "],\n";

        out << "  \"inheritance\": [";
        for (size_t i = 0; i < inh.size(); ++i) {
            const auto& e = inh[i];
            out << (i ? ",\n    " : "\n    ") << "{";
            bool first = true;
            field(out, "child", e.child, first);
            field(out, "parent", e.parent, first);
            field(out, "kind", e.kind, first);
            writeSourceRef(out, e.at, first);
            out << "}";
        }
        out << (inh.empty() ? "" : "\n  ") << "],\n";

        out << "  \"references\": [";
        for (size_t i = 0; i < refs.size(); ++i) {
            const auto& r = refs[i];
            out << (i ? ",\n    " : "\n    ") << "{";
            bool first = true;
            field(out, "target", r.target, first);
            field(out, "kind", r.kind, first);
            writeSourceRef(out, r.at, first);
            field(out, "from", r.from, first);
            out << "}";
        }
        out << (refs.empty() ? "" : "\n  ") << "],\n";

        out << "  \"overrides\": [";
        for (size_t i = 0; i < ovs.size(); ++i) {
            const auto& o = ovs[i];
            out << (i ? ",\n    " : "\n    ") << "{";
            bool first = true;
            field(out, "method", o.method, first);
            field(out, "overrides", o.overrides, first);
            writeSourceRef(out, o.at, first);
            out << "}";
        }
        out << (ovs.empty() ? "" : "\n  ") << "],\n";

        out << "  \"calls\": [";
        for (size_t i = 0; i < calls.size(); ++i) {
            const auto& c = calls[i];
            out << (i ? ",\n    " : "\n    ") << "{";
            bool first = true;
            field(out, "callee", c.callee, first);
            field(out, "caller", c.caller, first);
            if (c.isVirtual) {
                if (!first) out << ", ";
                out << "\"virtual\": true";
                first = false;
            }
            writeSourceRef(out, c.at, first);
            out << "}";
        }
        out << (calls.empty() ? "" : "\n  ") << "]\n";

        out << "}\n";
        return out.str();
    }

    bool XrefIndex::writeToFile(const std::string& path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        return f.good();
    }

    namespace {

        // Strip the source root prefix so recorded paths are root-relative — the
        // IDE resolves them against its own project root, not this machine's.
        std::string relativize(const std::string& file, const std::string& root) {
            if (root.empty() || file.empty()) return file;
            if (file.rfind(root, 0) != 0) return file;
            size_t at = root.size();
            while (at < file.size() && (file[at] == '/' || file[at] == '\\')) ++at;
            return file.substr(at);
        }

        // The modifiers an IDE actually renders. Deliberately a subset — this is a
        // navigation index, not a mirror of the type system.
        std::vector<std::string> modifierNames(Modifiable* m) {
            std::vector<std::string> out;
            if (!m) return out;
            static const std::pair<Modifier, const char*> kNamed[] = {
                {Modifier::PUBLIC,    "public"},
                {Modifier::PRIVATE,   "private"},
                {Modifier::PROTECTED, "protected"},
                {Modifier::STATIC,    "static"},
                {Modifier::FINAL,     "final"},
            };
            for (auto& [mod, name] : kNamed) {
                if (m->getModifiers().count(mod)) out.emplace_back(name);
            }
            return out;
        }

        std::string classKind(const CajetaClassPtr& c) {
            // CajetaView derives from CajetaClass, so a plain isInterface/isRecord
            // check silently reports every `view` as a `class`. That is a WRONG
            // value, not a missing one — the failure mode this whole export exists
            // to avoid — so discriminate on the concrete type.
            if (std::dynamic_pointer_cast<CajetaView>(c)) return "view";
            if (c->isInterface()) return "interface";
            if (c->isRecordType()) return "record";
            return "class";
        }

    } // namespace

    // ---- template-member capture (plan 1.5) --------------------------------
    namespace {
        thread_local bool gCaptureEnabled = false;
        thread_local std::vector<TemplateMember> gTemplateMembers;
    }

    void setCaptureEnabled(bool enabled) { gCaptureEnabled = enabled; }
    bool captureEnabled() { return gCaptureEnabled; }

    void resetCapture() { gTemplateMembers.clear(); }

    void registerTemplateMember(TemplateMember member) {
        gTemplateMembers.push_back(std::move(member));
    }

    void collectDeclarationsAndInheritance(XrefIndex& index,
                                           const std::string& sourceRoot) {
        index.setSourceRoot(sourceRoot);

        // Template members first — captured at parse time because the template's
        // body walk is skipped and it therefore holds no Method objects (1.5).
        for (const auto& tm : gTemplateMembers) {
            if (tm.at.line <= 0) continue;
            Declaration d;
            d.fqn         = tm.ownerFqn + "." + tm.name;
            d.kind        = tm.kind;
            d.owner       = tm.ownerFqn;
            d.overloadKey = tm.overloadKey;
            d.signature   = tm.overloadKey;
            d.at          = SourceRef{relativize(tm.at.file, sourceRoot),
                                      tm.at.line, tm.at.col};
            if (d.at.file.empty()) continue;
            index.addDeclaration(std::move(d));
        }

        // --- enums --------------------------------------------------------------
        // An enum is an i32-backed CajetaType carrying ENUM_FLAG, NOT a
        // CajetaClass, so the class walk below cannot see it. Handled first, and
        // separately, for exactly that reason (plan 1.4).
        for (auto& [mapKey, type] : CajetaType::getCanonicalMap()) {
            if (!type || !(type->getTypeFlags() & ENUM_FLAG)) continue;
            if (std::dynamic_pointer_cast<CajetaClass>(type)) continue;  // defensive

            const std::string canonical = type->getQName()->toCanonical();
            const std::string shortName = type->getQName()->getTypeName();
            const std::string file = relativize(type->getDeclaringFile(), sourceRoot);
            if (type->getDeclLine() <= 0 || file.empty()) continue;

            Declaration d;
            d.fqn  = canonical;
            d.kind = "enum";
            d.at   = SourceRef{file, type->getDeclLine(), type->getDeclColumn()};
            index.addDeclaration(std::move(d));

            // Constants. The ordinal registry is keyed by the enum's SHORT name;
            // the position registry is keyed the same way so the two stay in step.
            auto& positions = CajetaType::getEnumConstantPositions();
            auto pit = positions.find(shortName);
            if (pit == positions.end()) continue;
            for (auto& [constName, pos] : pit->second) {
                if (pos.line <= 0) continue;
                Declaration c;
                c.fqn   = canonical + "." + constName;
                c.kind  = "enumConstant";
                c.owner = canonical;
                c.at    = SourceRef{relativize(pos.file, sourceRoot), pos.line, pos.col};
                if (c.at.file.empty()) continue;
                index.addDeclaration(std::move(c));
            }
        }

        // --- classes, interfaces, records, views --------------------------------
        for (auto& [mapKey, type] : CajetaType::getCanonicalMap()) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
            if (!klass) continue;

            // NOT the map key. getCanonicalMap() is keyed by BOTH the canonical
            // FQN and the short name (so `ArrayList` and `cajeta.collection.
            // ArrayList` both resolve), which means every class is visited twice —
            // once under a bare name that is not a valid FQN. Ask the type for its
            // own canonical name instead; the writer's de-duplication then collapses
            // the second visit.
            const std::string canonical = klass->getQName()->toCanonical();

            // Skip INSTANTIATIONS (`demo.Box<int32>`). An instantiation is
            // monomorphized from its template and has no source of its own — its
            // declaration is the template's. Exporting one record per instantiation
            // would list the same source method once per element type, fragment
            // "who calls add" across instantiations, and name FQNs that appear in no
            // file. The template's members are captured separately (1.5).
            if (canonical.find('<') != std::string::npos) continue;

            const std::string file = relativize(klass->getDeclaringFile(), sourceRoot);
            // A type with no declaring position was synthesized (mock, template
            // placeholder, primitive box) — it has no source an IDE could open, so
            // it is not a declaration. Skip rather than emit a record pointing
            // nowhere: a bogus target is worse than a missing one.
            if (klass->getDeclLine() <= 0 || file.empty()) continue;

            Declaration d;
            d.fqn       = canonical;
            d.kind      = classKind(klass);
            d.modifiers = modifierNames(klass.get());
            d.at        = SourceRef{file, klass->getDeclLine(), klass->getDeclColumn()};
            index.addDeclaration(std::move(d));

            // --- inheritance: RESOLVED FQNs, never the raw declared name -------
            // This is the relation cajetadoc structurally cannot supply, and the
            // one the hierarchy, gutter, and virtual-call features rest on.
            for (auto& parent : klass->getSuperClasses()) {
                if (!parent) continue;
                InheritanceEdge e;
                e.child  = canonical;
                e.parent = parent->getQName()->toCanonical();
                e.kind   = "extends";
                e.at     = SourceRef{file, klass->getDeclLine(), klass->getDeclColumn()};
                index.addInheritance(std::move(e));
            }
            for (auto& iface : klass->getImplementedInterfaces()) {
                if (!iface) continue;
                InheritanceEdge e;
                e.child  = canonical;
                e.parent = iface->getQName()->toCanonical();
                e.kind   = "implements";
                e.at     = SourceRef{file, klass->getDeclLine(), klass->getDeclColumn()};
                index.addInheritance(std::move(e));
            }

            // --- members ------------------------------------------------------
            for (auto& [propName, prop] : klass->getProperties()) {
                if (!prop) continue;
                Declaration f;
                f.fqn       = canonical + "." + prop->getName();
                f.kind      = "field";
                f.owner     = canonical;
                f.modifiers = modifierNames(prop.get());
                f.at        = SourceRef{file, prop->getDeclLine(), prop->getDeclColumn()};
                if (f.at.line <= 0) continue;   // synthesized mirror field
                index.addDeclaration(std::move(f));
            }

            for (auto& [methodKey, method] : klass->getMethods()) {
                if (!method) continue;
                if (method->getDeclLine() <= 0) continue;   // synthesized (drops, mocks)

                Declaration m;
                m.kind  = (method->getName() == klass->getQName()->getTypeName())
                        ? "constructor" : "method";
                m.fqn   = canonical + "." + method->getName();
                m.owner = canonical;
                // The overload key is the compiler's OWN canonical unlabeled
                // signature — the same string whose FNV-1a hash the RTTI uses for
                // dispatch. Reusing it (rather than minting a fourth copy of
                // signatureHash, which the source warns must stay in lockstep)
                // means two same-arity overloads can never collide here.
                m.overloadKey = method->toCanonical(/*labeled=*/false);
                m.signature   = m.overloadKey;
                m.modifiers   = modifierNames(method.get());
                m.at = SourceRef{file, method->getDeclLine(), method->getDeclColumn()};
                index.addDeclaration(std::move(m));
            }
        }
    }

} // namespace cajeta::xref
