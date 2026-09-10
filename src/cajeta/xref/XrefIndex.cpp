#include "cajeta/xref/XrefIndex.h"

#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/StructureProperty.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/Annotatable.h"

#include "antlr4-runtime/antlr4-runtime.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>
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
            if (val.empty()) return;
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

        // Deterministic order by position; records that sort equal are duplicates.
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

        // One record as one JSON object. Shared by toJson and toNdjson so a record
        // renders identically in the document and in the stream.
        void writeRecord(std::ostringstream& out, const Declaration& d) {
            out << "{";
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
                first = false;
            }
            if (!d.annotations.empty()) {
                if (!first) out << ", ";
                out << "\"annotations\": [";
                for (size_t a = 0; a < d.annotations.size(); ++a) {
                    if (a) out << ", ";
                    out << "\"";
                    escapeInto(out, d.annotations[a]);
                    out << "\"";
                }
                out << "]";
                first = false;
            }
            out << "}";
        }

        void writeRecord(std::ostringstream& out, const InheritanceEdge& e) {
            out << "{";
            bool first = true;
            field(out, "child", e.child, first);
            field(out, "parent", e.parent, first);
            field(out, "kind", e.kind, first);
            writeSourceRef(out, e.at, first);
            out << "}";
        }

        void writeRecord(std::ostringstream& out, const Reference& r) {
            out << "{";
            bool first = true;
            field(out, "target", r.target, first);
            field(out, "kind", r.kind, first);
            writeSourceRef(out, r.at, first);
            field(out, "from", r.from, first);
            out << "}";
        }

        void writeRecord(std::ostringstream& out, const OverrideEdge& o) {
            out << "{";
            bool first = true;
            field(out, "method", o.method, first);
            field(out, "overrides", o.overrides, first);
            writeSourceRef(out, o.at, first);
            out << "}";
        }

        void writeRecord(std::ostringstream& out, const Call& c) {
            out << "{";
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

    } // namespace

    int XrefIndex::pruneDanglingEdges() {
        std::set<std::string> declared;
        for (const auto& d : declarations_) {
            if (!d.overloadKey.empty()) declared.insert(d.overloadKey);
            declared.insert(d.fqn);
        }

        const size_t before = calls_.size() + overrides_.size() + references_.size();

        calls_.erase(std::remove_if(calls_.begin(), calls_.end(),
            [&](const Call& c) { return !declared.count(c.callee); }), calls_.end());

        overrides_.erase(std::remove_if(overrides_.begin(), overrides_.end(),
            [&](const OverrideEdge& o) {
                return !declared.count(o.method) || !declared.count(o.overrides);
            }), overrides_.end());

        references_.erase(std::remove_if(references_.begin(), references_.end(),
            [&](const Reference& r) { return !declared.count(r.target); }),
            references_.end());

        const size_t after = calls_.size() + overrides_.size() + references_.size();
        return (int) (before - after);
    }

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
            out << (i ? ",\n    " : "\n    ");
            writeRecord(out, decls[i]);
        }
        out << (decls.empty() ? "" : "\n  ") << "],\n";

        out << "  \"inheritance\": [";
        for (size_t i = 0; i < inh.size(); ++i) {
            out << (i ? ",\n    " : "\n    ");
            writeRecord(out, inh[i]);
        }
        out << (inh.empty() ? "" : "\n  ") << "],\n";

        out << "  \"references\": [";
        for (size_t i = 0; i < refs.size(); ++i) {
            out << (i ? ",\n    " : "\n    ");
            writeRecord(out, refs[i]);
        }
        out << (refs.empty() ? "" : "\n  ") << "],\n";

        out << "  \"overrides\": [";
        for (size_t i = 0; i < ovs.size(); ++i) {
            out << (i ? ",\n    " : "\n    ");
            writeRecord(out, ovs[i]);
        }
        out << (ovs.empty() ? "" : "\n  ") << "],\n";

        out << "  \"calls\": [";
        for (size_t i = 0; i < calls.size(); ++i) {
            out << (i ? ",\n    " : "\n    ");
            writeRecord(out, calls[i]);
        }
        out << (calls.empty() ? "" : "\n  ") << "]\n";

        out << "}\n";
        return out.str();
    }

    std::string XrefIndex::toNdjson(const std::string& onlyFile,
                                    const std::string& reportAs) const {
        std::ostringstream out;
        out << "{\"kind\":\"xref\",\"rel\":\"version\",\"record\":{\"major\": "
            << kSchemaMajor << ", \"minor\": " << kSchemaMinor << "}}\n";

        auto emitAll = [&](const char* rel, auto records) {
            for (auto& r : records) {
                if (r.at.file != onlyFile) continue;
                r.at.file = reportAs;
                out << "{\"kind\":\"xref\",\"rel\":\"" << rel << "\",\"record\":";
                std::ostringstream rec;
                writeRecord(rec, r);
                out << rec.str() << "}\n";
            }
        };
        emitAll("declarations", sortedUnique(declarations_, declLess, declEq));
        emitAll("inheritance",  sortedUnique(inheritance_, edgeLess, edgeEq));
        emitAll("references",   sortedUnique(references_, refLess2, refEq2));
        emitAll("overrides",    sortedUnique(overrides_, ovLess, ovEq));
        emitAll("calls",        sortedUnique(calls_, callLess, callEq));
        return out.str();
    }

    bool XrefIndex::writeToFile(const std::string& path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        return f.good();
    }

    namespace {

        // Root-relative paths: the IDE resolves them against its own project root.
        std::string relativize(const std::string& file, const std::string& root) {
            if (root.empty() || file.empty()) return file;
            if (file.rfind(root, 0) != 0) return file;
            size_t at = root.size();
            while (at < file.size() && (file[at] == '/' || file[at] == '\\')) ++at;
            return file.substr(at);
        }

        // The modifiers an IDE renders: deliberately a subset of the type system.
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

        // Applied annotations as canonical FQNs where the annotation type is known
        // to the compiler. Compiler intrinsics, and any name that does not resolve,
        // are emitted AS WRITTEN rather than dropped.
        std::vector<std::string> annotationNames(Annotatable* a) {
            std::vector<std::string> out;
            if (!a) return out;
            for (auto& qName : a->getAnnotationList()) {
                if (!qName) continue;
                const std::string written = qName->toCanonical();
                if (written.empty()) continue;
                std::string resolved = written;
                if (auto type = CajetaType::find(written)) {
                    if (type->getQName()) resolved = type->getQName()->toCanonical();
                }
                out.push_back(resolved);
            }
            // Declaration order, minus duplicates: sorting would lose what was written.
            std::vector<std::string> deduped;
            for (auto& n : out) {
                if (std::find(deduped.begin(), deduped.end(), n) == deduped.end()) {
                    deduped.push_back(n);
                }
            }
            return deduped;
        }

        // The part of a canonical key identifying the METHOD, not its owner:
        // `demo.Dog::speak(pointer)` -> `speak(pointer)`. Two methods override iff
        // their suffixes match — same name AND same parameter types.
        std::string signatureSuffix(const std::string& canonicalKey) {
            auto at = canonicalKey.find("::");
            return at == std::string::npos ? canonicalKey
                                           : canonicalKey.substr(at + 2);
        }

        // Every ancestor of `c` — superclasses and interfaces, transitively.
        // Cycle-guarded: a corrupted hierarchy must not hang the compiler.
        void collectAncestors(const CajetaClassPtr& c,
                              std::vector<CajetaClassPtr>& out,
                              std::set<std::string>& seen) {
            if (!c) return;
            auto push = [&](const CajetaClassPtr& p) {
                if (!p) return;
                const std::string key = p->getQName()->toCanonical();
                if (!seen.insert(key).second) return;
                out.push_back(p);
                collectAncestors(p, out, seen);
            };
            for (auto& s : c->getSuperClasses()) push(s);
            for (auto& i : c->getImplementedInterfaces()) push(i);
        }

        // The rendered LABEL for a method, not its identity: `signature` renders,
        // `overloadKey` identifies. The canonical key is exact but unreadable — the
        // receiver leaks in as `pointer` and the return type is absent.
        std::string displaySignature(const MethodPtr& method, bool isCtor) {
            std::ostringstream s;
            if (!isCtor) {
                auto ret = method->getReturnType();
                s << (ret ? ret->toCanonical() : "void") << " ";
            }
            s << method->getName() << "(";
            bool first = true;
            for (auto& p : method->getParameterList()) {
                if (!p || p->getName() == "this") continue;
                if (!first) s << ", ";
                first = false;
                if (p->isTransferred()) s << "#";
                s << (p->getType() ? p->getType()->toCanonical() : "?");
                if (!p->getName().empty()) s << " " << p->getName();
            }
            s << ")";
            return s.str();
        }

        // The kind an IDE renders for `c`. Order is load-bearing: a view and an
        // annotation are each a CajetaClass too, so both are discriminated before
        // the isInterface/isRecord flags, which would report them as "class".
        std::string classKind(const CajetaClassPtr& c) {
            if (std::dynamic_pointer_cast<CajetaView>(c)) return "view";
            if (c->isAnnotation()) return "annotation";
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

    const std::string* internSourceFile(const std::string& name) {
        if (!gCaptureEnabled || name.empty()) return nullptr;
        if (name == antlr4::IntStream::UNKNOWN_SOURCE_NAME) return nullptr;

        // Node-based so addresses stay valid as the pool grows, and NEVER cleared:
        // AST nodes hold these pointers past any one compile's resetCapture().
        static std::mutex mu;
        static std::set<std::string> pool;
        std::lock_guard<std::mutex> lock(mu);
        return &*pool.insert(name).first;
    }

    // ---- call sites ---------------------------------------------------------
    namespace {
        struct OpenSite {
            std::string file;
            int line;
            int col;
            bool valid;   // false = a MASK; see CallSiteScope
        };
        thread_local std::vector<OpenSite> gOpenSites;
        thread_local std::vector<Call> gCalls;
        thread_local std::vector<Reference> gReferences;
    }

    // A node with no source file pushes a MASK rather than nothing: a synthesized
    // body is generated nested inside the user call that triggered it, so anything
    // it resolves would otherwise be attributed to that user's line.
    CallSiteScope::CallSiteScope(const std::string& file, int line, int col) {
        if (!gCaptureEnabled) return;
        const bool valid = !file.empty() && line > 0;
        gOpenSites.push_back(OpenSite{file, line, col, valid});
        pushed_ = true;
    }

    CallSiteScope::~CallSiteScope() {
        if (pushed_ && !gOpenSites.empty()) gOpenSites.pop_back();
    }

    SyntheticSourceScope::SyntheticSourceScope() {
        if (!gCaptureEnabled) return;
        gOpenSites.push_back(OpenSite{std::string(), 0, 0, /*valid=*/false});
        pushed_ = true;
    }

    SyntheticSourceScope::~SyntheticSourceScope() {
        if (pushed_ && !gOpenSites.empty()) gOpenSites.pop_back();
    }

    void noteResolvedCall(const std::string& calleeKey,
                          const std::string& callerKey,
                          bool isVirtual) {
        if (!gCaptureEnabled || calleeKey.empty() || gOpenSites.empty()) return;
        const OpenSite& site = gOpenSites.back();
        if (!site.valid) return;
        Call c;
        c.callee    = calleeKey;
        c.caller    = callerKey;
        c.isVirtual = isVirtual;
        c.at        = SourceRef{site.file, site.line, site.col};
        gCalls.push_back(std::move(c));
    }

    void drainCalls(XrefIndex& index, const std::string& sourceRoot) {
        // One site resolves more than once — resolve-types with no caller known,
        // then codegen — so collapse on (callee, site); a real caller always wins.
        std::map<std::string, Call> merged;
        for (const auto& c : gCalls) {
            const std::string file = relativize(c.at.file, sourceRoot);
            if (file.empty()) continue;

            const std::string key = c.callee + "|" + file + "|"
                                  + std::to_string(c.at.line) + "|"
                                  + std::to_string(c.at.col);
            auto it = merged.find(key);
            if (it == merged.end()) {
                Call out = c;
                out.at.file = file;
                merged.emplace(key, std::move(out));
                continue;
            }
            if (it->second.caller.empty() && !c.caller.empty()) {
                it->second.caller = c.caller;
            }
        }
        for (auto& [_, c] : merged) index.addCall(c);
    }

    void noteTypeReference(const std::string& targetFqn, const std::string& file,
                           int line, int col) {
        if (!gCaptureEnabled || targetFqn.empty() || file.empty() || line <= 0) return;
        Reference r;
        r.target = targetFqn;
        r.kind   = "type";
        r.at     = SourceRef{file, line, col};
        gReferences.push_back(std::move(r));
    }

    void noteFieldReference(const std::string& targetFqn, const std::string& file,
                            int line, int col) {
        if (!gCaptureEnabled || targetFqn.empty() || file.empty() || line <= 0) return;
        Reference r;
        r.target = targetFqn;
        r.kind   = "field";
        r.at     = SourceRef{file, line, col};
        gReferences.push_back(std::move(r));
    }

    void drainReferences(XrefIndex& index, const std::string& sourceRoot) {
        std::set<std::string> seen;
        for (const auto& r : gReferences) {
            const std::string file = relativize(r.at.file, sourceRoot);
            if (file.empty()) continue;
            const std::string key = r.target + "|" + file + "|"
                                  + std::to_string(r.at.line) + "|"
                                  + std::to_string(r.at.col);
            if (!seen.insert(key).second) continue;
            Reference out = r;
            out.at.file = file;
            index.addReference(std::move(out));
        }
    }

    namespace {
        // The capture logs as the stdlib prime left them; resetCapture restores
        // them instead of clearing, so a warm lint starts from that state.
        struct CaptureBaseline {
            bool valid = false;
            std::vector<TemplateMember> templateMembers;
            std::vector<Call> calls;
            std::vector<Reference> references;
        };
        thread_local CaptureBaseline gCaptureBaseline;
    }

    void captureBaseline() {
        gCaptureBaseline.templateMembers = gTemplateMembers;
        gCaptureBaseline.calls = gCalls;
        gCaptureBaseline.references = gReferences;
        gCaptureBaseline.valid = true;
    }

    void resetCapture() {
        gOpenSites.clear();
        if (gCaptureBaseline.valid) {
            gTemplateMembers = gCaptureBaseline.templateMembers;
            gCalls = gCaptureBaseline.calls;
            gReferences = gCaptureBaseline.references;
            return;
        }
        gTemplateMembers.clear();
        gCalls.clear();
        gReferences.clear();
    }

    void registerTemplateMember(TemplateMember member) {
        gTemplateMembers.push_back(std::move(member));
    }

    std::string templateKeyFor(const std::string& templateFqn,
                               const std::string& methodName,
                               int declaredParamCount) {
        const TemplateMember* hit = nullptr;
        for (const auto& tm : gTemplateMembers) {
            if (tm.ownerFqn != templateFqn || tm.name != methodName) continue;
            if (tm.kind == "field") continue;
            if (tm.declaredParams != declaredParamCount) continue;
            // Same name AND arity is unresolvable here: the instantiation's
            // parameter types are substituted and no longer comparable.
            if (hit) return "";
            hit = &tm;
        }
        return hit ? hit->overloadKey : std::string();
    }

    void collectDeclarationsAndInheritance(XrefIndex& index,
                                           const std::string& sourceRoot) {
        index.setSourceRoot(sourceRoot);

        // Template members first: a template's body walk is skipped, so it holds no
        // Method objects and the class walk below cannot see its members.
        for (const auto& tm : gTemplateMembers) {
            if (tm.at.line <= 0) continue;
            Declaration d;
            d.fqn         = tm.ownerFqn + "." + tm.name;
            d.kind        = tm.kind;
            d.owner       = tm.ownerFqn;
            d.overloadKey = tm.overloadKey;
            d.signature   = tm.signature;
            d.at          = SourceRef{relativize(tm.at.file, sourceRoot),
                                      tm.at.line, tm.at.col};
            if (d.at.file.empty()) continue;
            index.addDeclaration(std::move(d));
        }

        // --- enums --------------------------------------------------------------
        // An enum is an i32-backed CajetaType carrying ENUM_FLAG, not a
        // CajetaClass, so the class walk below cannot see it.
        for (auto& [mapKey, type] : CajetaType::getCanonicalMap()) {
            if (!type || !(type->getTypeFlags() & ENUM_FLAG)) continue;
            if (std::dynamic_pointer_cast<CajetaClass>(type)) continue;

            const std::string canonical = type->getQName()->toCanonical();
            const std::string shortName = type->getQName()->getTypeName();
            const std::string file = relativize(type->getDeclaringFile(), sourceRoot);
            if (type->getDeclLine() <= 0 || file.empty()) continue;

            Declaration d;
            d.fqn  = canonical;
            d.kind = "enum";
            d.at   = SourceRef{file, type->getDeclLine(), type->getDeclColumn()};
            index.addDeclaration(std::move(d));

            // Both enum registries are keyed by the SHORT name, so they stay in step.
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

            // NOT the map key: getCanonicalMap() is keyed by both the FQN and the
            // short name, so every class is visited twice, once under a bare name.
            const std::string canonical = klass->getQName()->toCanonical();

            // Skip INSTANTIATIONS (`demo.Box<int32>`): monomorphized from a
            // template, they have no source of their own, and the template's own
            // members are captured separately.
            if (canonical.find('<') != std::string::npos) continue;

            const std::string file = relativize(klass->getDeclaringFile(), sourceRoot);
            // No declaring position means synthesized — no source an IDE could open.
            if (klass->getDeclLine() <= 0 || file.empty()) continue;

            Declaration d;
            d.fqn       = canonical;
            d.kind        = classKind(klass);
            d.modifiers   = modifierNames(klass.get());
            d.annotations = annotationNames(klass.get());
            d.at        = SourceRef{file, klass->getDeclLine(), klass->getDeclColumn()};
            index.addDeclaration(std::move(d));

            // --- inheritance: RESOLVED FQNs, never the raw declared name -------
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
                f.modifiers   = modifierNames(prop.get());
                f.annotations = annotationNames(prop.get());
                if (prop->getType()) {
                    f.signature = prop->getType()->toCanonical() + " " + prop->getName();
                }
                f.at        = SourceRef{file, prop->getDeclLine(), prop->getDeclColumn()};
                if (f.at.line <= 0) continue;
                index.addDeclaration(std::move(f));
            }

            for (auto& [methodKey, method] : klass->getMethods()) {
                if (!method) continue;

                const bool isCtor =
                    method->getName() == klass->getQName()->getTypeName();

                // A method with no position was synthesized and is skipped, EXCEPT
                // an implicit constructor: `heap C()` is a real call site, so it is
                // declared at the class's own line rather than left dangling.
                int line = method->getDeclLine();
                int col  = method->getDeclColumn();
                if (line <= 0) {
                    if (!isCtor) continue;
                    line = klass->getDeclLine();
                    col  = klass->getDeclColumn();
                }

                Declaration m;
                m.kind  = isCtor ? "constructor" : "method";
                m.fqn   = canonical + "." + method->getName();
                m.owner = canonical;
                m.overloadKey = method->toCanonical(/*labeled=*/false);
                m.signature   = displaySignature(method, isCtor);
                m.modifiers   = modifierNames(method.get());
                m.annotations = annotationNames(method.get());
                m.at = SourceRef{file, line, col};
                index.addDeclaration(std::move(m));
            }

            // --- overrides: matched on the signature suffix, never on name alone -
            std::vector<CajetaClassPtr> ancestors;
            std::set<std::string> seenAncestors;
            collectAncestors(klass, ancestors, seenAncestors);
            if (ancestors.empty()) continue;

            for (auto& [methodKey, method] : klass->getMethods()) {
                if (!method || method->getDeclLine() <= 0) continue;
                const std::string myKey = method->toCanonical(/*labeled=*/false);
                const std::string mySig = signatureSuffix(myKey);

                // Nearest ancestor wins; `ancestors` is built parent-first.
                for (auto& anc : ancestors) {
                    bool found = false;
                    for (auto& [ancKey, ancMethod] : anc->getMethods()) {
                        if (!ancMethod) continue;
                        const std::string ancCanon =
                            ancMethod->toCanonical(/*labeled=*/false);
                        if (signatureSuffix(ancCanon) != mySig) continue;
                        if (ancCanon == myKey) continue;

                        OverrideEdge e;
                        e.method    = myKey;
                        e.overrides = ancCanon;
                        e.at = SourceRef{file, method->getDeclLine(),
                                         method->getDeclColumn()};
                        index.addOverride(std::move(e));
                        found = true;
                        break;
                    }
                    if (found) break;
                }
            }
        }
    }

} // namespace cajeta::xref
