#ifndef CAJETA_XREF_INDEX_H
#define CAJETA_XREF_INDEX_H

// The compiler's resolved view of a source root, source-mapped, as a
// machine-readable index for external tools (ide-symbol-index spec §2).
//
// Why this exists: the IntelliJ plugin parses Cajeta but does not understand it.
// The alternative — reimplementing Cajeta's name resolution, overload selection
// and override matching in Kotlin — has no oracle to be pinned against
// (`cajeta doc --emit-model-json` PARSES but does not RESOLVE; its
// extends/implements are raw declared names), and a resolver that silently
// disagrees with the compiler is worse than none: it sends you to the wrong
// declaration with total confidence, and renames the wrong thing. So the compiler
// exports what it already computed, and the IDE presents it.
//
// Contract: specs/schemas/cajeta-xref-v1.schema.json.
//
// Determinism is required (spec §2.0.7): the writer sorts every relation, so the
// same input yields byte-identical output.

#include <cstdint>
#include <string>
#include <vector>

namespace cajeta::xref {

    // Schema version. Bump MAJOR on any breaking change — consumers are required
    // to REFUSE an unknown major rather than guess at the contents.
    constexpr int kSchemaMajor = 1;
    constexpr int kSchemaMinor = 0;

    // A source span. Line is 1-based, column 0-based — the ANTLR convention the
    // compiler already carries on every AbstractSyntaxNode.
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
        // Identifies ONE overload: the compiler's own canonical unlabeled
        // signature. Two same-name, same-arity overloads MUST differ here — if
        // they collide, "who calls f(int32)" returns f(String)'s callers, and
        // renaming one overload rewrites the other's call sites.
        std::string overloadKey;
        std::vector<std::string> modifiers;
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

        // Drop every edge whose endpoint names a declaration this index does not
        // carry. Such an edge is a Ctrl-click into the void — the consumer resolves
        // it to nothing, or worse, to whatever later occupies that key. A missing
        // edge costs a navigation; a dangling one costs trust (spec §1.3).
        //
        // Call after every relation has been collected. Returns the number dropped.
        int pruneDanglingEdges();

        // Sort + de-duplicate every relation, then render. Deterministic by
        // construction: the same input yields byte-identical output.
        std::string toJson() const;

        // Render and write to `path`. Returns false (and leaves no partial file)
        // if the file cannot be opened.
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
    //
    // A template's body walk is SKIPPED (CajetaLlvmVisitor.h:726 — it "keeps the
    // template out of getAllMethods' codegen worklist by way of having no methods
    // at all"), so a generic class holds no Method objects and its members are
    // invisible to the declaration walk. Without this, `ArrayList.add` — the
    // most-called method in the stdlib — is simply not in the index.
    //
    // So the visitor captures a template's members declaratively at parse time:
    // name, position, and parameter types AS WRITTEN. No type resolution — that is
    // precisely what the skipped body walk cannot do, and navigation does not need
    // it. We record the TEMPLATE's member, never an instantiation's: an
    // instantiation is monomorphized from the template and has no source of its
    // own, so per-instantiation records would list one source method N times and
    // fragment "who calls add" across instantiations.
    struct TemplateMember {
        std::string ownerFqn;      // the template's canonical name, no type args
        std::string name;
        std::string kind;          // method | constructor | field
        std::string overloadKey;   // e.g. demo.Box::get(T) — params as written
        std::string signature;     // display label, e.g. `T get(int32 i)` (2.2.6)
        // Needed to map a resolved INSTANTIATION method back to this template
        // member. Method::getParameters() includes the receiver for instance
        // methods (which is why `greet()` keys as `greet(pointer)`), while a
        // declared parameter list does not — so the expected instantiation arity is
        // declaredParams + (isStatic ? 0 : 1).
        int declaredParams = 0;
        bool isStatic = false;
        SourceRef at;
    };

    // The template member key for a call resolved to an INSTANTIATION's method.
    // Returns "" when it cannot be determined unambiguously — omitting the edge,
    // never guessing at one.
    std::string templateKeyFor(const std::string& templateFqn,
                               const std::string& methodName,
                               int instantiationParamCount);

    // Off by default: a build that does not ask for xref captures nothing.
    void setCaptureEnabled(bool enabled);
    bool captureEnabled();
    // Clear per-compile state. Call at the start of a compile.
    void resetCapture();
    void registerTemplateMember(TemplateMember member);

    // ---- source-file interning (2.2.8) -------------------------------------
    //
    // An AST node's position is meaningless without the file it is a position IN.
    // The node used to take that file from the module being compiled at the moment
    // codegen reached it, which is right only by coincidence: a stdlib method body
    // is generated while a USER module is active, so `Optional.get`'s call sites
    // landed inside whichever demo file triggered the instantiation. 426 of 2589
    // call edges on samples/tour pointed at the wrong file, most at a line that
    // exists — a confident wrong answer, the one thing this index must never give.
    //
    // The token knows. Compiler::parseSource names each real-source stream, so a
    // node can record its origin at construction. A SYNTHETIC re-parse (template
    // instantiation, mock synthesis) names no stream, so its nodes get nullptr and
    // are excluded — correct, because a snippet's line numbers refer to the snippet.
    //
    // Returns a stable pointer (the pool is never cleared, so nodes may outlive any
    // one compile), or nullptr when the name is unknown or capture is off.
    const std::string* internSourceFile(const std::string& name);

    // ---- call sites (Unit 2) -----------------------------------------------
    //
    // `CajetaClass::resolveMethod` is the one choke point every callee resolution
    // passes through — but it knows the callee, not the CALL SITE. The AST node
    // does. So a node with a source position pushes it for the duration of its own
    // codegen, and resolveMethod attributes what it resolves to the innermost open
    // site.
    //
    // If nothing is pushed, nothing is recorded. That is the safe direction: a
    // missing call edge costs a navigation, a MISATTRIBUTED one sends "who calls
    // this" to the wrong line and renames the wrong code.
    class CallSiteScope {
    public:
        CallSiteScope(const std::string& file, int line, int col);
        ~CallSiteScope();
        CallSiteScope(const CallSiteScope&) = delete;
        CallSiteScope& operator=(const CallSiteScope&) = delete;
    private:
        bool pushed_ = false;
    };

    // Suppress all call recording for the duration — for a region that parses and
    // walks SYNTHESIZED source (template instantiation, method-template
    // instantiation, mock synthesis, the codec/@Logged body synthesizers).
    //
    // Necessary because such a region runs LAZILY, nested inside codegen of the
    // user call that triggered it, and it resolves callees during the walk — before
    // any node of its own opens a call site. Without a mask, `Stream<T>::fold`'s
    // internal calls to `Optional.get` were attributed to the user's `.fold(...)`
    // line, and `Csv.parse`'s 27 internal calls all to one line of CsvDemo. The
    // positions in a synthesized snippet refer to the snippet; they are not
    // anywhere, and "not anywhere" must not resolve to "wherever we happened to be".
    class SyntheticSourceScope {
    public:
        SyntheticSourceScope();
        ~SyntheticSourceScope();
        SyntheticSourceScope(const SyntheticSourceScope&) = delete;
        SyntheticSourceScope& operator=(const SyntheticSourceScope&) = delete;
    private:
        bool pushed_ = false;
    };

    // Record a resolved call at the innermost open call site. No-op when capture is
    // off, when no site is open, when the innermost site is masked, or when
    // `calleeKey` is empty.
    void noteResolvedCall(const std::string& calleeKey,
                          const std::string& callerKey,
                          bool isVirtual);

    // ---- references (2.1.5 / 2.2.2) ----------------------------------------
    //
    // A type name at a source position, resolved to the declaration it names.
    // Recorded from CajetaType::fromContext — the choke point every type name in
    // the language passes through, whatever its syntactic home (field type,
    // parameter, return, type argument, local, extends/implements).
    void noteTypeReference(const std::string& targetFqn, const std::string& file,
                           int line, int col);

    // A field/property access (`receiver.field`), resolved to the DECLARING class's
    // field — recorded from DotExpression, at the identifier's own token so the
    // reference sits under the caret the developer Ctrl-clicks.
    //
    // Deliberately not locals or parameters: those are the one thing the IDE
    // resolves for itself (spec §4.3), because a local is visible in the buffer the
    // developer is editing and needs no compiler round-trip to be correct.
    void noteFieldReference(const std::string& targetFqn, const std::string& file,
                            int line, int col);

    // Drain the calls and references recorded so far into an index.
    void drainCalls(XrefIndex& index, const std::string& sourceRoot);
    void drainReferences(XrefIndex& index, const std::string& sourceRoot);

    // Build the index for everything the compiler currently holds resolved:
    // walks the canonical type registry, emitting declarations and inheritance
    // edges (Unit 1), enums (1.4), and captured template members (1.5).
    // `sourceRoot` is stripped from recorded paths.
    void collectDeclarationsAndInheritance(XrefIndex& index,
                                           const std::string& sourceRoot);

} // namespace cajeta::xref

#endif // CAJETA_XREF_INDEX_H
