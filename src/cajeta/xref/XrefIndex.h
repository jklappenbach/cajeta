#ifndef CAJETA_XREF_INDEX_H
#define CAJETA_XREF_INDEX_H

// The compiler's resolved view of a source root, source-mapped, as a machine-
// readable index for external tools; the contract is
// specs/schemas/cajeta-xref-v1.schema.json and the output must be deterministic.

#include <cstdint>
#include <string>
#include <vector>

namespace cajeta::xref {

    // Bump MAJOR on a breaking change: consumers must REFUSE an unknown major.
    constexpr int kSchemaMajor = 1;
    constexpr int kSchemaMinor = 0;

    // A source position: line 1-based, column 0-based, the ANTLR convention.
    struct SourceRef {
        std::string file;   // relative to the source root
        int line = 0;
        int col = 0;

        bool valid() const { return !file.empty() && line > 0; }
    };

    struct Declaration {
        std::string fqn;
        std::string kind;         // class | interface | method | field | ...
        std::string owner;        // declaring type, for members
        std::string signature;    // methods/constructors
        // Identifies ONE overload. Two same-name, same-arity overloads MUST
        // differ here, or a rename rewrites the other one's call sites.
        std::string overloadKey;
        std::vector<std::string> modifiers;
        // Applied annotations by name only, canonical where the type resolves.
        std::vector<std::string> annotations;
        SourceRef at;
    };

    struct InheritanceEdge {
        std::string child;    // FQN
        std::string parent;   // RESOLVED FQN — never the raw declared name
        std::string kind;     // extends | implements
        SourceRef at;
    };

    struct Reference {      // Unit 2
        std::string target;
        std::string kind;
        std::string from;
        SourceRef at;
    };

    struct OverrideEdge {   // Unit 2
        std::string method;      // overloadKey of the overrider
        std::string overrides;   // overloadKey of the overridden
        SourceRef at;
    };

    struct Call {           // Unit 2
        std::string callee;      // overloadKey of the STATIC target
        std::string caller;      // overloadKey of the enclosing method
        bool isVirtual = false;
        SourceRef at;
    };

    class XrefIndex {
    public:
        void setSourceRoot(const std::string& root) { sourceRoot_ = root; }

        void addDeclaration(Declaration d) { declarations_.push_back(std::move(d)); }
        void addInheritance(InheritanceEdge e) { inheritance_.push_back(std::move(e)); }
        void addReference(Reference r) { references_.push_back(std::move(r)); }
        void addOverride(OverrideEdge o) { overrides_.push_back(std::move(o)); }
        void addCall(Call c) { calls_.push_back(std::move(c)); }

        // Drop every edge whose endpoint names a declaration this index lacks —
        // a dangling edge resolves to whatever later takes that key. Call once
        // every relation is collected; returns the number dropped.
        int pruneDanglingEdges();

        // Sort, de-duplicate and render; byte-identical for identical input.
        std::string toJson() const;

        // The lint-mode stream: one NDJSON line per record, wrapped by relation
        // to ride the diagnostic channel. Only records in `onlyFile`, reported
        // against `reportAs`. Opens with a version record; prune before calling.
        std::string toNdjson(const std::string& onlyFile,
                             const std::string& reportAs) const;

        // Render to `path`; false, and no partial file, if it cannot be opened.
        bool writeToFile(const std::string& path) const;

        bool empty() const {
            return declarations_.empty() && inheritance_.empty()
                && references_.empty() && overrides_.empty() && calls_.empty();
        }

    private:
        std::string sourceRoot_;
        std::vector<Declaration> declarations_;
        std::vector<InheritanceEdge> inheritance_;
        std::vector<Reference> references_;
        std::vector<OverrideEdge> overrides_;
        std::vector<Call> calls_;
    };

    // ---- template members (plan 1.5) ---------------------------------------
    // A template's body walk is skipped, so its members are captured at parse
    // time with parameter types AS WRITTEN. Always the TEMPLATE's member: an
    // instantiation has no source and would fragment "who calls add" N ways.
    struct TemplateMember {
        std::string ownerFqn;      // the template's canonical name, no type args
        std::string name;
        std::string kind;          // method | constructor | field
        std::string overloadKey;   // e.g. demo.Box::get(T) — params as written
        std::string signature;     // display label, e.g. `T get(int32 i)` (2.2.6)
        // The DECLARED count, receiver excluded. Callers must DROP a leading
        // `this`, never add one: carrying it depends on how the method resolved.
        int declaredParams = 0;
        bool isStatic = false;
        SourceRef at;
    };

    // The template member key for a call resolved to an INSTANTIATION's method;
    // `declaredParamCount` matches declaredParams exactly. "" when ambiguous.
    std::string templateKeyFor(const std::string& templateFqn,
                               const std::string& methodName,
                               int declaredParamCount);

    // Off by default: a build that does not ask for xref captures nothing.
    void setCaptureEnabled(bool enabled);
    bool captureEnabled();
    // Clear per-compile state at the start of a compile — or RESTORE a captured
    // baseline, so a warm lint starts where a fresh stdlib parse would.
    void resetCapture();
    // Snapshot the capture logs, with the logs' own thread affinity.
    void captureBaseline();
    void registerTemplateMember(TemplateMember member);

    // ---- source-file interning (2.2.8) -------------------------------------
    // A node's origin file comes from its own token stream, not the module
    // active during codegen; a synthetic re-parse names none and is excluded.
    // The returned pointer is stable (the pool is never cleared).
    const std::string* internSourceFile(const std::string& name);

    // ---- call sites (Unit 2) -----------------------------------------------
    // resolveMethod knows the callee, not the call site, so a node pushes its own
    // position for its codegen and resolutions attribute to the innermost site.
    class CallSiteScope {
    public:
        CallSiteScope(const std::string& file, int line, int col);
        ~CallSiteScope();
        CallSiteScope(const CallSiteScope&) = delete;
        CallSiteScope& operator=(const CallSiteScope&) = delete;
    private:
        bool pushed_ = false;
    };

    // Suppress call recording while walking SYNTHESIZED source: such a region
    // runs lazily inside the codegen of the call that triggered it, so without
    // the mask its internal calls land on the user's line.
    class SyntheticSourceScope {
    public:
        SyntheticSourceScope();
        ~SyntheticSourceScope();
        SyntheticSourceScope(const SyntheticSourceScope&) = delete;
        SyntheticSourceScope& operator=(const SyntheticSourceScope&) = delete;
    private:
        bool pushed_ = false;
    };

    // Record a resolved call at the innermost open call site; a no-op when
    // capture is off, no site is open, the site is masked, or the key is empty.
    void noteResolvedCall(const std::string& calleeKey,
                          const std::string& callerKey,
                          bool isVirtual);

    // ---- references (2.1.5 / 2.2.2) ----------------------------------------
    // A type name at a position, resolved to what it names. Recorded from
    // CajetaType::fromContext, which every type name passes through.
    void noteTypeReference(const std::string& targetFqn, const std::string& file,
                           int line, int col);

    // A `receiver.field` access, resolved to the DECLARING class's field, at the
    // identifier's own token. Locals and parameters: the IDE resolves those.
    void noteFieldReference(const std::string& targetFqn, const std::string& file,
                            int line, int col);

    // Drain the calls and references recorded so far into an index.
    void drainCalls(XrefIndex& index, const std::string& sourceRoot);
    void drainReferences(XrefIndex& index, const std::string& sourceRoot);

    // Walk the type registry into declarations, inheritance edges, enums and
    // captured template members, with `sourceRoot` stripped from paths.
    void collectDeclarationsAndInheritance(XrefIndex& index,
                                           const std::string& sourceRoot);

} // namespace cajeta::xref

#endif // CAJETA_XREF_INDEX_H
